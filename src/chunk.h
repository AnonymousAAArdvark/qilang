//
// Created by Troy Zhong on 8/29/21.
//

#ifndef QI_CHUNK_H
#define QI_CHUNK_H

#include "common.h"
#include "value.h"

typedef enum {
    OP_CONSTANT,
    OP_NIL,
    OP_TRUE,
    OP_FALSE,
    OP_POP,
    OP_GET_LOCAL,
    OP_SET_LOCAL,
    OP_GET_GLOBAL,
    OP_DEFINE_GLOBAL,
    OP_SET_GLOBAL,
    OP_GET_UPVALUE,
    OP_SET_UPVALUE,
    OP_GET_PROPERTY,
    OP_SET_PROPERTY,
    OP_GET_SUPER,
    OP_BUILD_LIST,
    OP_INDEX_SUBSCR,
    OP_STORE_SUBSCR,
    OP_EQUAL,
    OP_GREATER,
    OP_LESS,
    OP_ADD,
    OP_SUBTRACT,
    OP_BITWISE_NOT,
    OP_BITWISE_OR,
    OP_BITWISE_XOR,
    OP_BITWISE_AND,
    OP_BITWISE_LEFT_SHIFT,
    OP_BITWISE_RIGHT_SHIFT,
    OP_INCREMENT,
    OP_DECREMENT,
    OP_MULTIPLY,
    OP_DIVIDE,
    OP_MODULO,
    OP_NOT,
    OP_NEGATE,
    OP_JUMP,
    OP_JUMP_IF_FALSE,
    OP_LOOP,
    OP_CALL,
    OP_INVOKE,
    OP_SUPER_INVOKE,
    OP_CLOSURE,
    OP_CLOSE_UPVALUE,
    OP_RETURN,
    OP_CLASS,
    OP_INHERIT,
    OP_METHOD,
    OP_DUP,
    OP_DOUBLE_DUP,
    OP_END,
} OpCode;

typedef struct {
    int count;
    int capacity;
    uint8_t * code;
    int* lines;
    ValueArray constants;
} Chunk;

void initChunk(Chunk* chunk);
void freeChunk(Chunk* chunk);
void writeChunk(Chunk* chunk, uint8_t byte, int line);
int addConstant(Chunk* chunk, Value value);

#endif //QI_CHUNK_H