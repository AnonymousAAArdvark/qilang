//
// Created by Troy Zhong on 9/3/21.
//

#ifndef QI_TABLE_H
#define QI_TABLE_H

typedef struct {
    ObjString* key;
    Value value;
} Entry;

typedef struct {
    int count;
    int capacity;
    Entry* entries;
} Table;

void initTable(Table* table);
void freeTable(Table* table);
bool tableGet(Table* table, ObjString* key, Value* value);
bool tableSet(Table* table, ObjString* key, Value value);
bool tableDelete(Table* table, ObjString* key);
void tableAddAll(Table* from, Table* to);
ObjString* tableFindString(Table* table, const wchar_t* chars, int length, uint32_t hash);
void tableRemoveWhite(Table* table);
void markTable(Table* table);

#endif //QI_TABLE_H
