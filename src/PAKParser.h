#pragma once

#include <iostream>

#include "easylogging++.h"

using std::runtime_error;

#define HEADER_LENGTH 4
#define FILE_ENTRY_LENGTH 64

struct PAKHeader {
    char id[4];
    int32_t offset;
    int32_t size;
};

struct PAKFileEntry {
    char name[54];
    int32_t offset;
    int32_t size;
};

struct BSPEntry {
    int32_t offset;
    int32_t size;
};

struct BSPFile {
    int32_t version;
    BSPEntry entities;
    BSPEntry planes;
    BSPEntry miptex;
    BSPEntry vertices;
    BSPEntry visilist;
    BSPEntry nodes;
    BSPEntry texinfo;
    BSPEntry faces;
    BSPEntry lightmaps;
    BSPEntry clipnodes;
    BSPEntry leaves;
    BSPEntry lface;
    BSPEntry edges;
    BSPEntry ledges;
    BSPEntry models;
};

void parsePAK(const char*);