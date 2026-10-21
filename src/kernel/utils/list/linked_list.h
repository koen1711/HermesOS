#pragma once

#include <os/stddef.h>
#include <os/stdbool.h>

struct List
{
    struct List *next;
};

typedef struct List List;

typedef struct
{
    List *next;
    List *tail;
} HeadList;


void ListPushBack(HeadList *list, List *item);
List *ListPopFront(HeadList *list);
bool ListIsEmpty(HeadList *list);