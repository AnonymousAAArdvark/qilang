//
// Created by Troy Zhong on 8/29/21.
//

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <ctype.h>
#include <math.h>

#include "common.h"
#include "compiler.h"
#include "debug.h"
#include "object.h"
#include "memory.h"
#include "vm.h"
#include "core_module.h"

VM vm;

static void resetStack() {
    vm.stackTop = vm.stack;
    vm.frameCount = 0;
    vm.openUpvalues = NULL;
}

static void runtimeError(const wchar_t* format, ...) {
    va_list args;
    va_start(args, format);
    vfwprintf(stderr, format, args);
    va_end(args);
    fputs("\n", stderr);

    for (int i = vm.frameCount - 1; i >= 0; i--) {
        CallFrame* frame = &vm.frames[i];
        ObjFunction* function = frame->closure->function;
        size_t instruction = frame->ip - function->chunk.code - 1;
        fwprintf(stderr, L"【行 %d】在 ", function->chunk.lines[instruction]);
        if (function->name == NULL) {
            fwprintf(stderr, L"脚本\n");
        } else {
            fwprintf(stderr, L"%ls（）\n", function->name->chars);
        }
    }

    resetStack();
}

void defineNativeInstance(wchar_t* name, ObjInstance* instance) {
    push(OBJ_VAL(copyString(name, (int)wcslen(name))));
    push(OBJ_VAL(instance));
    tableSet(&vm.globals, AS_STRING(vm.stack[0]), vm.stack[1]);
    pop();
    pop();
}

void defineNative(const wchar_t* name, NativeFn function, int arity, ObjClass* klass) {
    push(OBJ_VAL(copyString(name, (int)wcslen(name))));
    push(OBJ_VAL(newNative(function, arity)));
    tableSet(&klass->methods, AS_STRING(vm.stack[0]), vm.stack[1]);
    pop();
    pop();
}

void defineProperty(const wchar_t* name, Value value, ObjInstance* instance) {
    push(OBJ_VAL(copyString(name, (int)wcslen(name))));
    push(value);
    tableSet(&instance->fields, AS_STRING(vm.stack[0]), vm.stack[1]);
    pop();
    pop();
}

void initVM() {
    resetStack();
    vm.objects = NULL;
    vm.bytesAllocated = 0;
    vm.nextGC = 1024 * 1024;

    vm.grayCount = 0;
    vm.grayCapacity = 0;
    vm.grayStack = NULL;

    initTable(&vm.globals);
    initTable(&vm.strings);

    vm.initString = NULL;
    vm.initString = copyString(L"初始化", 3);
    vm.markValue = true;

    initCoreClass();
}

void freeVM() {
    freeTable(&vm.globals);
    freeTable(&vm.strings);
    vm.initString = NULL;
    freeObjects();
}

bool isFalsey(Value value) {
    return IS_NIL(value) || (IS_BOOL(value) && !AS_BOOL(value));
}

void push(Value value) {
    *vm.stackTop = value;
    vm.stackTop++;
}

Value pop() {
    vm.stackTop--;
    return *vm.stackTop;
}

static Value peek(int distance) {
    return vm.stackTop[-1 - distance];
}

static bool containsChar(wchar_t* input, wchar_t c) {
    if (input == NULL) return iswspace(c);
    for (int i = 0; i < wcslen(input); i++)
        if (input[i] == c) return true;
    return false;
}

static bool call(ObjClosure* closure, int argCount) {
    if (argCount != closure->function->arity) {
        runtimeError(L"需要 %d 个参数，但得到 %d。", closure->function->arity, argCount);
        return false;
    }

    if (vm.frameCount == FRAMES_MAX) {
        runtimeError(L"堆栈溢出。");
        return false;
    }

    CallFrame* frame = &vm.frames[vm.frameCount++];
    frame->closure = closure;
    frame->ip = closure->function->chunk.code;
    frame->slots = vm.stackTop - argCount - 1;
    return true;
}

static bool callValue(Value callee, int argCount) {
    if (IS_OBJ(callee)) {
        switch (OBJ_TYPE(callee)) {
            case OBJ_BOUND_METHOD: {
                ObjBoundMethod* bound = AS_BOUND_METHOD(callee);
                vm.stackTop[-argCount - 1] = bound->receiver;
                return call(bound->method, argCount);
            }
            case OBJ_CLASS: {
                ObjClass* klass = AS_CLASS(callee);
                vm.stackTop[-argCount - 1] = OBJ_VAL(newInstance(klass, false));
                Value initializer;
                if (tableGet(&klass->methods, vm.initString, &initializer)) {
                    return call(AS_CLOSURE(initializer), argCount);
                } else if (argCount != 0) {
                    runtimeError(L"需要 0 个参数，但得到 %d。", argCount);
                    return false;
                }
                return true;
            }
            case OBJ_CLOSURE:
                return call(AS_CLOSURE(callee), argCount);
            default:
                break; // Non-callable object type.
        }
    }
    runtimeError(L"只能调用功能和类。");
    return false;
}

static bool invokeFromClass(ObjClass* klass, bool isStatic, ObjString* name, int argCount, CallFrame* frame, uint8_t* ip) {
    Value method;
    if (!tableGet(&klass->methods, name, &method)) {
        frame->ip = ip;
        runtimeError(L"未定义的属性「%ls」。", name->chars);
        return false;
    }
    if (!isStatic) return call(AS_CLOSURE(method), argCount);

    ObjNative* native = AS_NATIVE(method);
    if (native->arity != -1 && argCount != native->arity) {
        runtimeError(L"需要 %d 个参数，但得到 %d。", native->arity, argCount);
        return false;
    }
    if (native->function(argCount, vm.stackTop - argCount)) {
        vm.stackTop -= argCount;
        return true;
    } else {
        if (vm.frameCount != 0) runtimeError(AS_STRING(vm.stackTop[-argCount - 1])->chars);
        return false;
    }
}

static bool invokeInstance(const Value* receiver, ObjString* name, int argCount, CallFrame* frame, uint8_t* ip) {
    ObjInstance* instance = AS_INSTANCE(*receiver);

    Value value;
    if (tableGet(&instance->fields, name, &value)) {
        vm.stackTop[-argCount - 1] = value;
        return callValue(value, argCount);
    }

    return invokeFromClass(instance->klass, instance->isStatic, name, argCount, frame, ip);
}

static bool invokeString(const Value* receiver, ObjString* name, int argCount, CallFrame* frame, uint8_t* ip) {
    if (wcscmp(name->chars, L"长度") == 0) {
        // Returns the length of the string
        if (argCount != 0) {
            frame->ip = ip;
            runtimeError(L"需要 0 个参数，但得到 %d。", argCount);
            return false;
        }

        vm.stackTop -= argCount + 1;
        push(NUMBER_VAL(AS_STRING(*receiver)->length));
        return true;
    } else if (wcscmp(name->chars, L"指数") == 0) {
        // Returns the index of the first char matching the input string
        ObjString* str = AS_STRING(*receiver);
        if (argCount != 1) {
            frame->ip = ip;
            runtimeError(L"需要 1 个参数，但得到 %d。", argCount);
            return false;
        } else if (!IS_STRING(peek(argCount - 1))) {
            frame->ip = ip;
            runtimeError(L"参数 1（开头）的类型必须时「字符串」，而不是「%ls」。", getType(vm.stackTop[-argCount]));
            return false;
        }

        ObjString* search = AS_STRING(peek(argCount - 1));
        wchar_t* found = wcsstr(str->chars, search->chars);
        vm.stackTop -= argCount + 1;

        push(NUMBER_VAL(found == NULL ? -1 : found - str->chars));

        return true;
    } else if (wcscmp(name->chars, L"计数") == 0) {
        // Returns the amount of times the input string was found
        ObjString* str = AS_STRING(*receiver);
        if (argCount != 1) {
            frame->ip = ip;
            runtimeError(L"需要 1 个参数，但得到 %d。", argCount);
            return false;
        } else if (!IS_STRING(peek(argCount - 1))) {
            frame->ip = ip;
            runtimeError(L"参数 1（开头）的类型必须时「字符串」，而不是「%ls」。", getType(vm.stackTop[-argCount]));
            return false;
        }

        ObjString* search = AS_STRING(peek(argCount - 1));
        double count = 0;
        const wchar_t* tmp = wcsstr(str->chars, search->chars);
        while (tmp) {
            count++;
            tmp++;
            tmp = wcsstr(tmp, search->chars);
        }
        vm.stackTop -= argCount + 1;

        push(NUMBER_VAL(count));

        return true;
    } else if (wcscmp(name->chars, L"拆分") == 0) {
        // Returns a split string as a list.
        ObjString* str = AS_STRING(*receiver);
        if (argCount != 1) {
            frame->ip = ip;
            runtimeError(L"需要 1 个参数，但得到 %d。", argCount);
            return false;
        } else if (!IS_STRING(peek(argCount - 1))) {
            frame->ip = ip;
            runtimeError(L"参数 1（开头）的类型必须时「字符串」，而不是「%ls」。", getType(vm.stackTop[-argCount]));
            return false;
        }

        ObjString* search = AS_STRING(peek(argCount - 1));
        ObjList* list = newList();
        wchar_t *last, *token, *tmp, *toFree;
        toFree = tmp = wcsdup(str->chars);

        token = wcstok(tmp, search->chars, &last);
        while (token != NULL) {
            insertToList(list, OBJ_VAL(copyString(token, wcslen(token))), list->count);
            token = wcstok(NULL, search->chars, &last);
        }

        free(toFree);
        vm.stackTop -= argCount + 1;

        push(OBJ_VAL(list));

        return true;
    } else if (wcscmp(name->chars, L"替换") == 0) {
        // Returns a string with all occurrences of the 1st argument replaced with the 2nd argument.
        ObjString* str = AS_STRING(*receiver);
        if (argCount != 2) {
            frame->ip = ip;
            runtimeError(L"需要 2 个参数，但得到 %d。", argCount);
            return false;
        } else if (!IS_STRING(peek(argCount - 1))) {
            frame->ip = ip;
            runtimeError(L"参数 1（开头）的类型必须时「字符串」，而不是「%ls」。", getType(vm.stackTop[-argCount]));
            return false;
        } else if (!IS_STRING(peek(argCount - 2))) {
            frame->ip = ip;
            runtimeError(L"参数 2（结尾）的类型必须时「字符串」，而不是「%ls」。", getType(vm.stackTop[-argCount]));
            return false;
        }

        ObjString* old = AS_STRING(peek(argCount - 1));
        ObjString* new = AS_STRING(peek(argCount - 2));
        wchar_t *buff, *next;
        buff = wcsdup(str->chars);
        int pos;

        wchar_t* found = wcsstr(str->chars, old->chars);
        pos = found == NULL ? -1 : (int)(found - str->chars);
        if (pos != -1) {
            buff[0] = 0;
            wcsncpy(buff, str->chars, pos);
            buff[pos] = 0;
            wcscat(buff, new->chars);
            next = str->chars + pos + wcslen(old->chars);

            while (wcslen(next) != 0) {
                found = wcsstr(next, old->chars);
                pos = found == NULL ? -1 : (int)(found - next);
                if (pos == -1) {
                    wcscat(buff, next);
                    break;
                }
                wcsncat(buff, next, pos);
                wcscat(buff, new->chars);
                next = next + pos + wcslen(old->chars);
            }
        }

        vm.stackTop -= argCount + 1;
        push(OBJ_VAL(copyString(buff, wcslen(buff))));

        return true;
    } else if (wcscmp(name->chars, L"修剪") == 0) {
        // Returns a string with whitespace or chars of given string removed from the start and end of the input string
        const wchar_t* str = AS_STRING(*receiver)->chars;
        if (argCount > 1) {
            frame->ip = ip;
            runtimeError(L"需要 0 到 1 个参数，但得到 %d。", argCount);
            return false;
        } else if (argCount == 1 && !IS_STRING(peek(argCount - 1))) {
            frame->ip = ip;
            runtimeError(L"参数 1（开头）的类型必须时「字符串」，而不是「%ls」。", getType(vm.stackTop[-argCount]));
            return false;
        }

        wchar_t* remove = argCount ? AS_STRING(peek(argCount - 1))->chars : NULL;
        const wchar_t* end;
        size_t res_size;
        while(containsChar(remove, (wchar_t)*str)) str++;

        if(*str == 0) {
            vm.stackTop -= argCount + 1;
            push(OBJ_VAL(copyString(0, 1)));
            return true;
        }

        end = str + wcslen(str) - 1;
        while(end > str && containsChar(remove, (wchar_t)*end)) end--;
        end++;

        res_size = (end - str) < wcslen(str)-1 ? (end - str) : wcslen(str)-1;
        vm.stackTop -= argCount + 1;
        push(OBJ_VAL(copyString(str, res_size)));
        return true;
    } else if (wcscmp(name->chars, L"修剪始") == 0) {
        // Returns a string with whitespace or chars of given string removed from the start of the input string
        const wchar_t* str = AS_STRING(*receiver)->chars;
        if (argCount > 1) {
            frame->ip = ip;
            runtimeError(L"需要 0 到 1 个参数，但得到 %d。", argCount);
            return false;
        } else if (argCount == 1 && !IS_STRING(peek(argCount - 1))) {
            frame->ip = ip;
            runtimeError(L"参数 1（开头）的类型必须时「字符串」，而不是「%ls」。", getType(vm.stackTop[-argCount]));
            return false;
        }

        wchar_t* remove = argCount ? AS_STRING(peek(argCount - 1))->chars : NULL;
        size_t res_size;
        while(containsChar(remove, (wchar_t)*str)) str++;

        if(*str == 0) {
            vm.stackTop -= argCount + 1;
            push(OBJ_VAL(copyString(0, 1)));
            return true;
        }

        vm.stackTop -= argCount + 1;
        push(OBJ_VAL(copyString(str, wcslen(str))));
        return true;
    } else if (wcscmp(name->chars, L"修剪端") == 0) {
        // Returns a string with whitespace or chars of given string removed from the end of the input string
        const wchar_t* str = AS_STRING(*receiver)->chars;
        if (argCount > 1) {
            frame->ip = ip;
            runtimeError(L"需要 0 到 1 个参数，但得到 %d。", argCount);
            return false;
        } else if (argCount == 1 && !IS_STRING(peek(argCount - 1))) {
            frame->ip = ip;
            runtimeError(L"参数 1（开头）的类型必须时「字符串」，而不是「%ls」。", getType(vm.stackTop[-argCount]));
            return false;
        }

        wchar_t* remove = argCount ? AS_STRING(peek(argCount - 1))->chars : NULL;
        const wchar_t* end;
        size_t res_size;

        end = str + wcslen(str) - 1;
        while(end > str && containsChar(remove, (wchar_t)*end)) end--;
        end++;

        res_size = (end - str) < wcslen(str)-1 ? (end - str) : wcslen(str)-1;
        vm.stackTop -= argCount + 1;
        push(OBJ_VAL(copyString(str, res_size)));
        return true;
    } else if (wcscmp(name->chars, L"大写") == 0) {
        // Returns a string where all characters are in upper case.
        ObjString* str = AS_STRING(*receiver);
        if (argCount != 0) {
            frame->ip = ip;
            runtimeError(L"需要 0 个参数，但得到 %d。", argCount);
            return false;
        }

        wchar_t* chars = ALLOCATE(wchar_t, str->length + 1);
        wcscpy(chars, str->chars);
        chars[str->length + 1] = L'\0';
        wchar_t* c = chars;
        while (*c) {
            *c = towupper(*c);
            c++;
        }
        ObjString* result = takeString(chars, str->length + 1);

        vm.stackTop -= argCount + 1;
        push(OBJ_VAL(result));
        return true;
    } else if (wcscmp(name->chars, L"小写") == 0) {
        // Returns a string where all characters are in lower case.
        ObjString* str = AS_STRING(*receiver);
        if (argCount != 0) {
            frame->ip = ip;
            runtimeError(L"需要 0 个参数，但得到 %d。", argCount);
            return false;
        }

        wchar_t* chars = ALLOCATE(wchar_t, str->length + 1);
        wcscpy(chars, str->chars);
        chars[str->length + 1] = L'\0';
        wchar_t* c = chars;
        while (*c) {
            *c = towlower(*c);
            c++;
        }
        ObjString* result = takeString(chars, str->length + 1);

        vm.stackTop -= argCount + 1;
        push(OBJ_VAL(result));
        return true;
    } else if (wcscmp(name->chars, L"子串") == 0) {
        // Returns a part of a string between given indexes
        if (argCount != 2) {
            frame->ip = ip;
            runtimeError(L"需要 2 个参数，但得到 %d。", argCount);
            return false;
        } else if (!IS_NUMBER(peek(argCount - 1))) {
            frame->ip = ip;
            runtimeError(L"参数 1（开头）的类型必须时「数字」，而不是「%ls」。", getType(vm.stackTop[-argCount]));
            return false;
        } else if (!IS_NUMBER(peek(argCount - 2))) {
            frame->ip = ip;
            runtimeError(L"参数 2（结尾）的类型必须时「数字」，而不是「%ls」。", getType(vm.stackTop[-argCount]));
            return false;
        }

        ObjString* str = AS_STRING(*receiver);
        int begin = AS_NUMBER(peek(argCount - 1));
        int end = AS_NUMBER(peek(argCount - 2));
        if (begin < 0) begin = str->length + begin;
        if (end < 0) end = str->length + end;

        if (!isValidStringIndex(str, begin)) {
            frame->ip = ip;
            runtimeError(L"参数 1 不是有效索引。");
            return false;
        } else if (!isValidStringIndex(str, end - 1)) { // Ending index is exclusive
            frame->ip = ip;
            runtimeError(L"参数 2 不是有效索引。");
            return false;
        } else if (end < begin) {
            frame->ip = ip;
            runtimeError(L"结束索引不能在开始索引之前。");
            return false;
        }

        wchar_t* chars = ALLOCATE(wchar_t, end - begin + 1);
        memcpy( chars, &str->chars[begin], (end - begin) * sizeof(wchar_t) );
        chars[end - begin] = L'\0';
        ObjString* result = takeString(chars, end - begin + 1);

        vm.stackTop -= argCount + 1;
        push(OBJ_VAL(result));
        return true;
    }
    frame->ip = ip;
    runtimeError(L"未定义的属性「%ls」。", name->chars);
    return false;
}

static bool invokeList(const Value* receiver, ObjString* name, int argCount, CallFrame* frame, uint8_t* ip) {
    if (wcscmp(name->chars, L"推") == 0) {
        // Push a value to the end of a list increasing the list's length by 1
        if (argCount != 1) {
            frame->ip = ip;
            runtimeError(L"需要 1 个参数，但得到 %d。", argCount);
            return false;
        }
        ObjList *list = AS_LIST(*receiver);
        Value item = peek(argCount - 1);
        insertToList(list, item, list->count);
        vm.stackTop -= argCount + 1;
        push(NIL_VAL);
        return true;
    } else if (wcscmp(name->chars, L"弹") == 0) {
        // Pop a value from the end of a list decreasing the list's length by 1
        if (argCount != 0) {
            frame->ip = ip;
            runtimeError(L"需要 0 个参数，但得到 %d。", argCount);
            return false;
        }

        ObjList *list = AS_LIST(*receiver);

        if (!isValidListIndex(list, list->count - 1)) {
            frame->ip = ip;
            runtimeError(L"无法从空列表中弹出。");
            return false;
        }

        deleteFromList(list, list->count - 1);
        vm.stackTop -= argCount + 1;
        push(NIL_VAL);
        return true;
    } else if (wcscmp(name->chars, L"插") == 0) {
        // Insert a value to the specified index of a list increasing the list's length by 1
        if (argCount != 2) {
            frame->ip = ip;
            runtimeError(L"需要 2 个参数，但得到 %d。", argCount);
            return false;
        } else if (!IS_NUMBER(peek(argCount - 1))) {
            frame->ip = ip;
            runtimeError(L"参数 1（索引）的类型必须时「数字」，而不是「%ls」。", getType(vm.stackTop[-argCount]));
            return false;
        }

        ObjList *list = AS_LIST(*receiver);
        int index = AS_NUMBER(peek(argCount - 1));
        if (index < 0) index = list->count + index;
        Value item = peek(argCount - 2);

        if (!isValidListIndex(list, index)) {
            frame->ip = ip;
            runtimeError(L"参数 1 不是有效索引");
            return false;
        }

        insertToList(list, item, index);
        vm.stackTop -= argCount + 1;
        push(NIL_VAL);
        return true;
    } else if (wcscmp(name->chars, L"删") == 0) {
        // Delete an item from a list at the given index.
        if (argCount != 1) {
            frame->ip = ip;
            runtimeError(L"需要 1 个参数，但得到 %d。", argCount);
            return false;
        } else if (!IS_NUMBER(peek(argCount - 1))) {
            frame->ip = ip;
            runtimeError(L"参数 1（索引）的类型必须时「数字」，而不是「%ls」。", getType(vm.stackTop[-argCount]));
            return false;
        }

        ObjList* list = AS_LIST(*receiver);
        int index = AS_NUMBER(peek(argCount - 1));
        if (index < 0) index = list->count + index;

        if (!isValidListIndex(list, index)) {
            frame->ip = ip;
            runtimeError(L"参数 1 不是有效索引。");
            return false;
        }

        deleteFromList(list, index);
        vm.stackTop -= argCount + 1;
        push(NIL_VAL);
        return true;
    } else if (wcscmp(name->chars, L"长度") == 0) {
        // Returns the length of the list
        if (argCount != 0) {
            frame->ip = ip;
            runtimeError(L"需要 0 个参数，但得到 %d。", argCount);
            return false;
        }
        vm.stackTop -= argCount + 1;
        push(NUMBER_VAL(AS_LIST(*receiver)->count));
        return true;
    } else if (wcscmp(name->chars, L"过滤") == 0) {
        // Filters the list based on the given function
        if (argCount != 1) {
            frame->ip = ip;
            runtimeError(L"需要 1 个参数，但得到 %d。", argCount);
            return false;
        } else if (!IS_CLOSURE(peek(argCount - 1))) {
            frame->ip = ip;
            runtimeError(L"参数 1（测试）的类型必须时「关闭」，而不是「%ls」。", getType(vm.stackTop[-argCount]));
            return false;
        }


        ObjList* list = AS_LIST(*receiver);
        ObjList* filtered = newList();
        ObjClosure* closure = AS_CLOSURE(peek(argCount - 1));

        if (closure->function->arity != 1) {
            frame->ip = ip;
            runtimeError(L"输入功能需要 1 个参数，但得到 %d。", argCount);
            return false;
        }
        for (int i = 0; i < list->count; i++) {
            Value ret;
            Value argArr[1] = {indexFromList(list, i)};
            if (runClosure(closure, &ret, argArr, 1) != INTERPRET_OK) {
                return false;
            }
            if (!isFalsey(ret)) insertToList(filtered, indexFromList(list, i), filtered->count);
        }

        vm.stackTop -= argCount + 1;
        push(OBJ_VAL(filtered));
        return true;
    } else if (wcscmp(name->chars, L"排序") == 0) {
        // Sorts the list based on the given function or in ascending order
        if (argCount > 1) {
            frame->ip = ip;
            runtimeError(L"需要 0 或 1 个参数，但得到 %d。", argCount);
            return false;
        } else if (argCount == 1 && !IS_CLOSURE(peek(argCount - 1))) {
            frame->ip = ip;
            runtimeError(L"参数 1（测试）的类型必须时「关闭」，而不是「%ls」。", getType(vm.stackTop[-argCount]));
            return false;
        }

        ObjList* list = AS_LIST(*receiver);
        ObjClosure* closure = argCount == 1 ? AS_CLOSURE(peek(argCount - 1)) : NULL;

        if (closure && closure->function->arity != 2) {
            frame->ip = ip;
            runtimeError(L"输入功能需要 2 个参数，但得到 %d。", argCount);
            return false;
        }

        if (!sortList(list, 0, list->count - 1, closure))
            return false;

        vm.stackTop -= argCount + 1;
        push(OBJ_VAL(list));
        return true;
    }

    frame->ip = ip;
    runtimeError(L"未定义的属性「%ls」。", name->chars);
    return false;
}

static bool invoke(ObjString* name, int argCount, CallFrame* frame, uint8_t* ip) {
    Value receiver = peek(argCount);

    if (IS_INSTANCE(receiver)) {
        return invokeInstance(&receiver, name, argCount, frame, ip);
    } else if (IS_STRING(receiver)) {
        return invokeString(&receiver, name, argCount, frame, ip);
    } else if (IS_LIST(receiver)) {
        return invokeList(&receiver, name, argCount, frame, ip);
    }

    frame->ip = ip;
    runtimeError(L"只有实例、字符串和列表有方法。");
    return false;
}

static bool bindMethod(ObjClass* klass, ObjString* name, CallFrame* frame, uint8_t* ip) {
    Value method;
    if (!tableGet(&klass->methods, name, &method)) {
        frame->ip = ip;
        runtimeError(L"未定义的属性「%ls」。", name->chars);
        return false;
    }
    ObjBoundMethod* bound;
    if (IS_NATIVE(method))
        bound = newBoundNative(peek(0), AS_NATIVE(method));
    else
        bound = newBoundMethod(peek(0), AS_CLOSURE(method));
    pop();
    push(OBJ_VAL(bound));
    return true;
}

static ObjUpvalue* captureUpvalue(Value* local) {
    ObjUpvalue* prevUpvalue = NULL;
    ObjUpvalue* upvalue = vm.openUpvalues;
    while (upvalue != NULL && upvalue->location > local) {
        prevUpvalue = upvalue;
        upvalue = upvalue->next;
    }

    if (upvalue != NULL && upvalue->location == local) {
        return upvalue;
    }

    ObjUpvalue* createdUpvalue = newUpvalue(local);
    createdUpvalue->next = upvalue;

    if (prevUpvalue == NULL) {
        vm.openUpvalues = createdUpvalue;
    } else {
        prevUpvalue->next = createdUpvalue;
    }
    return createdUpvalue;
}

static void closeUpvalues(const Value* last) {
    while (vm.openUpvalues != NULL && vm.openUpvalues->location >= last) {
        ObjUpvalue* upvalue = vm.openUpvalues;
        upvalue->closed = *upvalue->location;
        upvalue->location = &upvalue->closed;
        vm.openUpvalues = upvalue->next;
    }
}

static void defineMethod(ObjString* name) {
    Value method = peek(0);
    ObjClass* klass = AS_CLASS(peek(1));
    tableSet(&klass->methods, name, method);
    pop();
}

static ObjString* concatenate(ObjString* a, ObjString* b) {
    int length = a->length + b->length;
    wchar_t* chars = ALLOCATE(wchar_t, length + 1);
    memcpy(chars, a->chars, a->length * sizeof(wchar_t));
    memcpy(chars + a->length, b->chars, b->length * sizeof(wchar_t));
    chars[length] = L'\0';

    ObjString* result = takeString(chars, length);
    return result;
}

static InterpretResult run() {
    CallFrame* frame = &vm.frames[vm.frameCount - 1];
    register uint8_t* ip = frame->ip;

#define READ_BYTE() (*ip++)

#define READ_SHORT() \
    (ip += 2, (uint16_t)((ip[-2] << 8) | ip[-1]))

#define READ_CONSTANT() \
    (frame->closure->function->chunk.constants.values[READ_BYTE()])

#define READ_STRING() AS_STRING(READ_CONSTANT())
#define BINARY_OP(valueType, op) \
    do { \
      if (!IS_NUMBER(peek(0)) || !IS_NUMBER(peek(1))) { \
        frame->ip = ip; \
        runtimeError(L"操作数必须是数字。"); \
        return INTERPRET_RUNTIME_ERROR; \
      } \
      double b = AS_NUMBER(pop()); \
      double a = AS_NUMBER(pop()); \
      push(valueType(a op b)); \
    } while (false)
#define BINARY_BITWISE_OP(valueType, op) \
    do { \
      if (!IS_NUMBER(peek(0)) || !IS_NUMBER(peek(1))) { \
        frame->ip = ip; \
        runtimeError(L"操作数必须是数字。"); \
        return INTERPRET_RUNTIME_ERROR; \
      } \
      int32_t b = (int32_t)AS_NUMBER(pop()); \
      int32_t a = (int32_t)AS_NUMBER(pop()); \
      push(valueType(a op b)); \
    } while (false)
#define BINARY_FUNC_OP(valueType, op) \
    do { \
      if (!IS_NUMBER(peek(0)) || !IS_NUMBER(peek(1))) { \
        frame->ip = ip; \
        runtimeError(L"操作数必须是数字。"); \
        return INTERPRET_RUNTIME_ERROR; \
      } \
      double b = AS_NUMBER(pop()); \
      double a = AS_NUMBER(pop());    \
      push(valueType(op(a, b))); \
    } while (false)

    for(;;) {
#ifdef DEBUG_TRACE_EXECUTION
        wprintf(L"          ");
        for (Value *slot = vm.stack; slot < vm.stackTop; slot++) {
            wprintf(L"[ ");
            printValue(*slot);
            wprintf(L" ]");
        }
        wprintf(L"\n");
        disassembleInstruction(&frame->closure->function->chunk,
                               (int) (frame->ip - frame->closure->function->chunk.code));
#endif
        switch (READ_BYTE()) {
            case OP_CONSTANT: {
                Value constant = READ_CONSTANT();
                push(constant);
                break;
            }
            case OP_NIL:
                push(NIL_VAL);
                break;
            case OP_TRUE:
                push(BOOL_VAL(true));
                break;
            case OP_FALSE:
                push(BOOL_VAL(false));
                break;
            case OP_POP:
                pop();
                break;
            case OP_SET_LOCAL: {
                uint8_t slot = READ_BYTE();
                frame->slots[slot] = peek(0);
                break;
            }
            case OP_GET_LOCAL: {
                uint8_t slot = READ_BYTE();
                push(frame->slots[slot]);
                break;
            }
            case OP_GET_GLOBAL: {
                ObjString *name = READ_STRING();
                Value value;
                if (!tableGet(&vm.globals, name, &value)) {
                    frame->ip = ip;
                    runtimeError(L"未定义的变量「%ls」。", name->chars);
                    return INTERPRET_RUNTIME_ERROR;
                }
                push(value);
                break;
            }
            case OP_DEFINE_GLOBAL: {
                ObjString *name = READ_STRING();
                tableSet(&vm.globals, name, peek(0));
                pop();
                break;
            }
            case OP_SET_GLOBAL: {
                ObjString *name = READ_STRING();

                if (tableSet(&vm.globals, name, peek(0))) {
                    tableDelete(&vm.globals, name);
                    frame->ip = ip;
                    runtimeError(L"未定义的变量「%ls」。", name->chars);
                    return INTERPRET_RUNTIME_ERROR;
                }
                break;
            }
            case OP_GET_UPVALUE: {
                uint8_t slot = READ_BYTE();
                push(*frame->closure->upvalues[slot]->location);
                break;
            }
            case OP_SET_UPVALUE: {
                uint8_t slot = READ_BYTE();
                *frame->closure->upvalues[slot]->location = peek(0);
                break;
            }
            case OP_GET_PROPERTY: {
                if (!IS_INSTANCE(peek(0))) {
                    frame->ip = ip;
                    runtimeError(L"只有实例有属性。");
                    return INTERPRET_RUNTIME_ERROR;
                }
                ObjInstance *instance = AS_INSTANCE(peek(0));
                ObjString *name = READ_STRING();

                Value value;
                if (tableGet(&instance->fields, name, &value)) {
                    pop(); // Instance.
                    push(value);
                    break;
                }

                if (!bindMethod(instance->klass, name, frame, ip)) {
                    return INTERPRET_RUNTIME_ERROR;
                }
                break;
            }
            case OP_SET_PROPERTY: {
                if (!IS_INSTANCE(peek(1))) {
                    frame->ip = ip;
                    runtimeError(L"只有实例有字段。");
                    return INTERPRET_RUNTIME_ERROR;
                }

                ObjInstance *instance = AS_INSTANCE(peek(1));
                if (instance->isStatic) {
                    frame->ip = ip;
                    runtimeError(L"不能修改常量属性。");
                    return INTERPRET_RUNTIME_ERROR;
                }

                tableSet(&instance->fields, READ_STRING(), peek(0));
                Value value = pop();
                pop();
                push(value);
                break;
            }
            case OP_GET_SUPER: {
                ObjString *name = READ_STRING();
                ObjClass *superclass = AS_CLASS(pop());

                if (!bindMethod(superclass, name, frame, ip)) {
                    return INTERPRET_RUNTIME_ERROR;
                }
                break;
            }
            case OP_EQUAL: {
                Value b = pop();
                Value a = pop();
                push(BOOL_VAL(valuesEqual(a, b)));
                break;
            }
            case OP_GREATER:
                BINARY_OP(BOOL_VAL, >);
                break;
            case OP_LESS:
                BINARY_OP(BOOL_VAL, <);
                break;
            case OP_ADD:
                if (IS_STRING(peek(0)) && IS_STRING(peek(1))) {
                    ObjString* b = AS_STRING(peek(0));
                    ObjString* a = AS_STRING(peek(1));
                    ObjString* result = concatenate(a, b);
                    pop();
                    pop();
                    push(OBJ_VAL(result));
                } else if (IS_NUMBER(peek(0)) && IS_NUMBER(peek(1))) {
                    double b = AS_NUMBER(pop());
                    double a = AS_NUMBER(pop());
                    push(NUMBER_VAL(a + b));
                } else {
                    frame->ip = ip;
                    runtimeError(L"操作数必须是两个数字或两个字符串。");
                    return INTERPRET_RUNTIME_ERROR;
                }
                break;
            case OP_SUBTRACT:
                BINARY_OP(NUMBER_VAL, -);
                break;
            case OP_MULTIPLY:
                BINARY_OP(NUMBER_VAL, *);
                break;
            case OP_DIVIDE:
                BINARY_OP(NUMBER_VAL, /);
                break;
            case OP_MODULO:
                BINARY_FUNC_OP(NUMBER_VAL, fmod);
                break;
            case OP_BITWISE_AND:
                BINARY_BITWISE_OP(NUMBER_VAL, &);
                break;
            case OP_BITWISE_OR:
                BINARY_BITWISE_OP(NUMBER_VAL, |);
                break;
            case OP_BITWISE_XOR:
                BINARY_BITWISE_OP(NUMBER_VAL, ^);
                break;
            case OP_BITWISE_LEFT_SHIFT:
                BINARY_BITWISE_OP(NUMBER_VAL, <<);
                break;
            case OP_BITWISE_RIGHT_SHIFT:
                BINARY_BITWISE_OP(NUMBER_VAL, >>);
                break;
            case OP_NOT:
                push(BOOL_VAL(isFalsey(pop())));
                break;
            case OP_NEGATE:
                if (!IS_NUMBER(peek(0))) {
                    frame->ip = ip;
                    runtimeError(L"操作数必须是数字。");
                    return INTERPRET_RUNTIME_ERROR;
                }
                push(NUMBER_VAL(-AS_NUMBER(pop())));
                break;
            case OP_BITWISE_NOT:
                if (!IS_NUMBER(peek(0))) {
                    frame->ip = ip;
                    runtimeError(L"操作数必须是数字。");
                    return INTERPRET_RUNTIME_ERROR;
                }
                push(NUMBER_VAL(~(int32_t)AS_NUMBER(pop())));
                break;
            case OP_INCREMENT: {
                if (!IS_NUMBER(peek(0))) {
                    frame->ip = ip;
                    runtimeError(L"操作数必须是数字。");
                    return INTERPRET_RUNTIME_ERROR;
                }
                push(NUMBER_VAL(AS_NUMBER(pop()) + 1));
                break;
            }
            case OP_DECREMENT: {
                if (!IS_NUMBER(peek(0))) {
                    frame->ip = ip;
                    runtimeError(L"操作数必须是数字。");
                    return INTERPRET_RUNTIME_ERROR;
                }
                push(NUMBER_VAL(AS_NUMBER(pop()) - 1));
                break;
            }
            case OP_JUMP: {
                uint16_t offset = READ_SHORT();
                ip += offset;
                break;
            }
            case OP_JUMP_IF_FALSE: {
                uint16_t offset = READ_SHORT();
                if (isFalsey(peek(0))) ip += offset;
                break;
            }
            case OP_LOOP: {
                uint16_t offset = READ_SHORT();
                ip -= offset;
                break;
            }
            case OP_CALL: {
                int argCount = READ_BYTE();
                frame->ip = ip;
                if (!callValue(peek(argCount), argCount)) {
                    return INTERPRET_RUNTIME_ERROR;
                }
                frame = &vm.frames[vm.frameCount - 1];
                ip = frame->ip;
                break;
            }
            case OP_INVOKE: {
                ObjString *method = READ_STRING();
                int argCount = READ_BYTE();
                frame->ip = ip;
                if (!invoke(method, argCount, frame, ip)) {
                    return INTERPRET_RUNTIME_ERROR;
                }
                frame = &vm.frames[vm.frameCount - 1];
                ip = frame->ip;
                break;
            }
            case OP_SUPER_INVOKE: {
                ObjString *method = READ_STRING();
                int argCount = READ_BYTE();
                frame->ip = ip;
                ObjClass *superclass = AS_CLASS(pop());
                if (!invokeFromClass(superclass, false, method, argCount, frame, ip)) {
                    return INTERPRET_RUNTIME_ERROR;
                }
                frame = &vm.frames[vm.frameCount - 1];
                ip = frame->ip;
                break;
            }
            case OP_CLOSURE: {
                ObjFunction *function = AS_FUNCTION(READ_CONSTANT());
                ObjClosure *closure = newClosure(function);
                push(OBJ_VAL(closure));
                for (int i = 0; i < closure->upvalueCount; i++) {
                    uint8_t isLocal = READ_BYTE();
                    uint8_t index = READ_BYTE();
                    if (isLocal) {
                        closure->upvalues[i] = captureUpvalue(frame->slots + index);
                    } else {
                        closure->upvalues[i] = frame->closure->upvalues[index];
                    }
                }
                break;
            }
            case OP_CLOSE_UPVALUE:
                closeUpvalues(vm.stackTop - 1);
                pop();
                break;
            case OP_RETURN: {
                Value result = pop();
                closeUpvalues(frame->slots);
                vm.frameCount--;

                if (vm.frameCount == 0) {
                    pop();
                    return INTERPRET_OK;
                } else if (frame->callClosure) {
                    push(result);
                    frame->callClosure = false;
                    return INTERPRET_OK;
                }

                vm.stackTop = frame->slots;
                push(result);
                frame = &vm.frames[vm.frameCount - 1];
                ip = frame->ip;
                break;
            }
            case OP_CLASS:
                push(OBJ_VAL(newClass(READ_STRING())));
                break;
            case OP_INHERIT: {
                Value superclass = peek(1);
                if (!IS_CLASS(superclass)) {
                    frame->ip = ip;
                    runtimeError(L"超类必须是个类。");
                    return INTERPRET_RUNTIME_ERROR;
                }
                ObjClass *subclass = AS_CLASS(peek(0));
                tableAddAll(&AS_CLASS(superclass)->methods, &subclass->methods);
                pop(); // Subclass.
                break;
            }
            case OP_METHOD:
                defineMethod(READ_STRING());
                break;
            case OP_DUP: push(peek(0)); break;
            case OP_DOUBLE_DUP: push(peek(1)); push(peek(1)); break;
            case OP_BUILD_LIST: {
                // Stack before: [item1, item2, ..., itemN] and after: [list]
                ObjList* list = newList();
                uint8_t itemCount = READ_BYTE();

                // Add items to list
                push(OBJ_VAL(list)); // So list isn't sweeped by GC in insertToList
                for (int i = itemCount; i > 0; i--) {
                    insertToList(list, peek(i), list->count);
                }
                pop();

                // Pop items from stack
                while (itemCount-- > 0) {
                    pop();
                }

                push(OBJ_VAL(list));
                break;
            }
            case OP_INDEX_SUBSCR: {
                // Stack before: [list, index] and after: [index(list, index)]
                Value index = pop();
                Value obj = pop();

                if (IS_STRING(obj)) {
                    ObjString *objString = AS_STRING(obj);

                    if (!IS_NUMBER(index)) {
                        frame->ip = ip;
                        runtimeError(L"字符串索引不是数字。");
                        return INTERPRET_RUNTIME_ERROR;
                    }
                    int numIndex = AS_NUMBER(index);
                    if (numIndex < 0) numIndex = objString->length + numIndex;

                    if (!isValidStringIndex(objString, numIndex)) {
                        frame->ip = ip;
                        runtimeError(L"字符串索引超出范围。");
                        return INTERPRET_RUNTIME_ERROR;
                    }
                    wchar_t* result = ALLOCATE(wchar_t, 1);
                    result[0] = indexFromString(objString, numIndex);
                    push(OBJ_VAL(takeString(result, 1)));
                    break;
                } else if (IS_LIST(obj)) {
                    ObjList *objList = AS_LIST(obj);

                    if (!IS_NUMBER(index)) {
                        frame->ip = ip;
                        runtimeError(L"列表索引不是数字。");
                        return INTERPRET_RUNTIME_ERROR;
                    }
                    int numIndex = AS_NUMBER(index);
                    if (numIndex < 0) numIndex = objList->count + numIndex;

                    if (!isValidListIndex(objList, numIndex)) {
                        frame->ip = ip;
                        runtimeError(L"列表索引超出范围。");
                        return INTERPRET_RUNTIME_ERROR;
                    }

                    Value result = indexFromList(objList, numIndex);
                    push(result);
                    break;
                }

                frame->ip = ip;
                runtimeError(L"无效类型索引到。");
                return INTERPRET_RUNTIME_ERROR;
            }
            case OP_STORE_SUBSCR: {
                // Stack before: [list, index, item] and after: [item]
                Value item = pop();
                Value index = pop();
                Value obj = pop();

                if (IS_STRING(obj)) {
                    ObjString* objString = AS_STRING(obj);

                    if (!IS_NUMBER(index)) {
                        frame->ip = ip;
                        runtimeError(L"字符串索引不是数字。");
                        return INTERPRET_RUNTIME_ERROR;
                    } else if (!IS_STRING(item)) {
                        frame->ip = ip;
                        runtimeError(L"字符串中只能存储字符。");
                        return INTERPRET_RUNTIME_ERROR;
                    }

                    ObjString* itemString = AS_STRING(item);
                    int numIndex = AS_NUMBER(index);
                    if (numIndex < 0) numIndex = objString->length + numIndex;

                    if (!isValidStringIndex(objString, numIndex)) {
                        frame->ip = ip;
                        runtimeError(L"字符串索引无效。");
                        return INTERPRET_RUNTIME_ERROR;
                    } else if (wcslen(itemString->chars) != 1) {
                        frame->ip = ip;
                        runtimeError(
                                L"期望长度为 1 的字符串，但长度为 %d。", wcslen(itemString->chars));
                        return INTERPRET_RUNTIME_ERROR;
                    }

                    storeToString(objString, numIndex, itemString->chars[0]);
                    push(item);
                    break;
                } else if (IS_LIST(obj)) {
                    ObjList *objList = AS_LIST(obj);

                    if (!IS_NUMBER(index)) {
                        frame->ip = ip;
                        runtimeError(L"列表索引不是数字。");
                        return INTERPRET_RUNTIME_ERROR;
                    }
                    int numIndex = AS_NUMBER(index);
                    if (numIndex < 0) numIndex = objList->count + numIndex;

                    if (!isValidListIndex(objList, numIndex)) {
                        frame->ip = ip;
                        runtimeError(L"列表索引无效。");
                        return INTERPRET_RUNTIME_ERROR;
                    }

                    storeToList(objList, numIndex, item);
                    push(item);
                    break;
                }

                frame->ip = ip;
                runtimeError(L"无法存储值：变量不是字符串或列表。");
                return INTERPRET_RUNTIME_ERROR;
            }
        }
    }

#undef READ_BYTE
#undef READ_SHORT
#undef READ_CONSTANT
#undef READ_STRING
#undef BINARY_FUNC_OP
#undef BINARY_OP
}

InterpretResult runClosure(ObjClosure* closure, Value* value, Value args[], int argCount) {
    for (int i = 0; i < argCount; i++) {
        push(args[i]);
    }
    call(closure, argCount);
    vm.frames[vm.frameCount - 1].callClosure = true;
    InterpretResult result = run();
    *value = pop();
    vm.stackTop -= argCount;
    return result;
}

InterpretResult interpret(const char* source) {
    wchar_t* wsource = ALLOCATE(wchar_t, strlen(source) + 1);
    mbstowcs(wsource, source, strlen(source) + 1);
    ObjFunction* function = compile(wsource);
    if (function == NULL) return INTERPRET_COMPILE_ERROR;

    push(OBJ_VAL(function));
    ObjClosure* closure = newClosure(function);
    pop();
    push(OBJ_VAL(closure));
    call(closure, 0);

    return run();
}