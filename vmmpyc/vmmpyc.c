// vmmpyc.c : implementation MemProcFS/VMM Python API
//
// (c) Ulf Frisk, 2018-2019
// Author: Ulf Frisk, pcileech@frizk.net
//
#define Py_LIMITED_API 0x03060000
#ifdef _DEBUG
#undef _DEBUG
#include <python.h>
#define _DEBUG
#else
#include <python.h>
#endif
#include <ws2tcpip.h>
#include <Windows.h>
#include "vmmdll.h"

inline int PyDict_SetItemString_DECREF(PyObject *dp, const char *key, PyObject *item)
{
    int i = PyDict_SetItemString(dp, key, item);
    Py_XDECREF(item);
    return i;
}

inline int PyList_Append_DECREF(PyObject *dp, PyObject *item)
{
    int i = PyList_Append(dp, item);
    Py_XDECREF(item);
    return i;
}

//-----------------------------------------------------------------------------
// UTIL FUNCTIONS BELOW:
//-----------------------------------------------------------------------------

VOID Util_FileTime2String(_In_ PFILETIME pFileTime, _Out_writes_(MAX_PATH) LPSTR szTime)
{
    SYSTEMTIME SystemTime;
    if(!*(PQWORD)pFileTime) {
        strcpy_s(szTime, MAX_PATH, "                    ***");
        return;
    }
    FileTimeToSystemTime(pFileTime, &SystemTime);
    sprintf_s(
        szTime,
        MAX_PATH,
        "%04i-%02i-%02i %02i:%02i:%02i UTC",
        SystemTime.wYear,
        SystemTime.wMonth,
        SystemTime.wDay,
        SystemTime.wHour,
        SystemTime.wMinute,
        SystemTime.wSecond
    );
}



//-----------------------------------------------------------------------------
// INITIALIZATION FUNCTIONALITY BELOW:
// Choose one way of initialzing the VMM / Memory Process File System.
//-----------------------------------------------------------------------------

// [STR] -> None
static PyObject*
VMMPYC_Initialize(PyObject *self, PyObject *args)
{
    PyObject *pyList, *pyString, **pyBytesDstArgs;
    BOOL result;
    DWORD i, cDstArgs;
    LPSTR *pszDstArgs;
    if(!PyArg_ParseTuple(args, "O!", &PyList_Type, &pyList)) { return NULL; } // borrowed reference
    cDstArgs = (DWORD)PyList_Size(pyList);
    if(cDstArgs == 0) { 
        Py_DECREF(pyList);
        return PyErr_Format(PyExc_RuntimeError, "VMMPYC_Initialize: Required argument list is empty.");
    }
    // allocate & initialize buffer+basic
    pszDstArgs = (LPSTR*)LocalAlloc(LMEM_ZEROINIT, sizeof(LPSTR) * cDstArgs);
    pyBytesDstArgs = (PyObject**)LocalAlloc(LMEM_ZEROINIT, sizeof(PyObject*) * cDstArgs);
    if(!pszDstArgs || !pyBytesDstArgs) {
        Py_DECREF(pyList);
        return PyErr_NoMemory();
    }
    // iterate over # entries and build argument list
    for(i = 0; i < cDstArgs; i++) {
        pyString = PyList_GetItem(pyList, i);   // borrowed reference
        if(!PyUnicode_Check(pyString)) { 
            Py_DECREF(pyList);
            return PyErr_Format(PyExc_RuntimeError, "VMMPYC_Initialize: Argument list contains non string item.");
        }
        pyBytesDstArgs[i] = PyUnicode_AsEncodedString(pyString, NULL, NULL);
        pszDstArgs[i] = pyBytesDstArgs[i] ? PyBytes_AsString(pyBytesDstArgs[i]) : "";

    }
    Py_DECREF(pyList);
    result = VMMDLL_Initialize(cDstArgs, pszDstArgs);
    for(i = 0; i < cDstArgs; i++) {
        Py_XDECREF(pyBytesDstArgs[i]);
    }
    if(!result) { return PyErr_Format(PyExc_RuntimeError, "VMMPYC_Initialize: Initialization of VMM failed."); }
    return Py_BuildValue("s", NULL);    // None returned on success.
}

// () -> None
static PyObject*
VMMPYC_Close(PyObject *self, PyObject *args)
{
    Py_BEGIN_ALLOW_THREADS;
    VMMDLL_Close();
    Py_END_ALLOW_THREADS;
    return Py_BuildValue("s", NULL);    // None returned on success.
}

// (DWORD) -> None
static PyObject*
VMMPYC_Refresh(PyObject *self, PyObject *args)
{
    BOOL result;
    DWORD dwReserved = 0;
    if(!PyArg_ParseTuple(args, "k", &dwReserved)) { return NULL; }
    Py_BEGIN_ALLOW_THREADS;
    result = VMMDLL_Refresh(dwReserved);
    Py_END_ALLOW_THREADS;
    if(!result) { return PyErr_Format(PyExc_RuntimeError, "VMMPYC_Refresh: Refresh failed."); }
    return Py_BuildValue("s", NULL);    // None returned on success.
}



//-----------------------------------------------------------------------------
// CONFIGURATION SETTINGS BELOW:
// Configure the memory process file system or the underlying memory
// acquisition devices.
//-----------------------------------------------------------------------------

// (ULONG64) -> ULONG64
static PyObject*
VMMPYC_ConfigGet(PyObject *self, PyObject *args)
{
    BOOL result;
    ULONG64 fOption, qwValue = 0;
    if(!PyArg_ParseTuple(args, "K", &fOption)) { return NULL; }
    Py_BEGIN_ALLOW_THREADS;
    result = VMMDLL_ConfigGet(fOption, &qwValue);
    Py_END_ALLOW_THREADS;
    if(!result) { return PyErr_Format(PyExc_RuntimeError, "VMMPYC_ConfigGet: Unable to retrieve config value for setting."); }
    return PyLong_FromUnsignedLongLong(qwValue);
}

// (ULONG64, ULONG64) -> None
static PyObject*
VMMPYC_ConfigSet(PyObject *self, PyObject *args)
{
    BOOL result;
    ULONG64 fOption, qwValue = 0;
    if(!PyArg_ParseTuple(args, "KK", &fOption, &qwValue)) { return NULL; }
    Py_BEGIN_ALLOW_THREADS;
    result = VMMDLL_ConfigSet(fOption, qwValue);
    Py_END_ALLOW_THREADS;
    if(!result) { return PyErr_Format(PyExc_RuntimeError, "VMMPYC_ConfigSet: Unable to set config value for setting."); }
    return Py_BuildValue("s", NULL);    // None returned on success.
}



//-----------------------------------------------------------------------------
// VMMPYC C-PYTHON FUNCTIONS BELOW:
//-----------------------------------------------------------------------------

// (DWORD, [STR], (DWORD)) -> [{...}]
static PyObject*
VMMPYC_MemReadScatter(PyObject *self, PyObject *args)
{
    PyObject *pyListSrc, *pyListItemSrc, *pyListDst, *pyDict;
    BOOL result;
    DWORD dwPID, cMEMs, flags = 0;
    ULONG64 i, qwA;
    PMEM_IO_SCATTER_HEADER pMEM, pMEMs;
    PPMEM_IO_SCATTER_HEADER ppMEMs;
    PBYTE pb, pbDataBuffer;
    if(!PyArg_ParseTuple(args, "kO!|k", &dwPID, &PyList_Type, &pyListSrc, &flags)) { return NULL; } // borrowed reference
    cMEMs = (DWORD)PyList_Size(pyListSrc);
    if(cMEMs == 0) { 
        Py_DECREF(pyListSrc);
        return PyList_New(0);
    }
    // allocate & initialize buffer+basic
    pb = LocalAlloc(0, cMEMs * (sizeof(PMEM_IO_SCATTER_HEADER) + sizeof(MEM_IO_SCATTER_HEADER) + 0x1000));
    if(!pb) {
        Py_DECREF(pyListSrc);
        return PyErr_NoMemory();
    }
    ppMEMs = (PPMEM_IO_SCATTER_HEADER)pb;
    pMEMs = (PMEM_IO_SCATTER_HEADER)(pb + cMEMs * sizeof(PMEM_IO_SCATTER_HEADER));
    pbDataBuffer = pb + cMEMs * (sizeof(PMEM_IO_SCATTER_HEADER) + sizeof(MEM_IO_SCATTER_HEADER));
    ZeroMemory(pb, pbDataBuffer - pb);
    // iterate over # entries and build scatter data structure
    for(i = 0; i < cMEMs; i++) {
        pMEM = pMEMs + i;
        pyListItemSrc = PyList_GetItem(pyListSrc, i); // borrowed reference
        if(!pyListItemSrc || !PyLong_Check(pyListItemSrc)) {
            Py_DECREF(pyListSrc);
            LocalFree(pb);
            return PyErr_Format(PyExc_RuntimeError, "VMMPYC_MemReadScatter: Argument list contains non numeric item.");
        }
        qwA = PyLong_AsUnsignedLongLong(pyListItemSrc);
        if(qwA == (ULONG64)-1) {
            Py_DECREF(pyListSrc);
            LocalFree(pb);
            return PyErr_Format(PyExc_RuntimeError, "VMMPYC_MemReadScatter: Argument list contains out-of-range numeric item.");
        }
        pMEM->cbMax = 0x1000;
        pMEM->pb = pbDataBuffer + (i << 12);
        pMEM->qwA = qwA;
        ppMEMs[i] = pMEM;
    }
    Py_DECREF(pyListSrc);
    // call c-dll for vmm
    Py_BEGIN_ALLOW_THREADS;
    result = VMMDLL_MemReadScatter(dwPID, ppMEMs, cMEMs, flags);
    Py_END_ALLOW_THREADS;
    if(!result) {
        LocalFree(pb);
        return PyErr_Format(PyExc_RuntimeError, "VMMPYC_MemReadScatter: Failed.");
    }
    if(!(pyListDst = PyList_New(0))) {
        LocalFree(pb);
        return PyErr_NoMemory();
    }
    for(i = 0; i < cMEMs; i++) {
        pMEM = pMEMs + i;
        if((pyDict = PyDict_New())) {
            PyDict_SetItemString_DECREF(pyDict, "addr", PyLong_FromUnsignedLongLong(pMEM->qwA));
            PyDict_SetItemString_DECREF(pyDict, ((dwPID == -1) ? "pa" : "va"), PyLong_FromUnsignedLongLong(pMEM->qwA));
            PyDict_SetItemString_DECREF(pyDict, "data", PyBytes_FromStringAndSize(pMEM->pb, 0x1000));
            PyDict_SetItemString_DECREF(pyDict, "size", PyLong_FromUnsignedLong(pMEM->cb));
            PyList_Append_DECREF(pyListDst, pyDict);
        }
    }
    LocalFree(pb);
    return pyListDst;
}

// (DWORD, ULONG64, DWORD, (ULONG64)) -> PBYTE
static PyObject*
VMMPYC_MemRead(PyObject *self, PyObject *args)
{
    PyObject *pyBytes;
    BOOL result;
    DWORD dwPID, cb, cbRead = 0;
    ULONG64 qwA, flags = 0;
    PBYTE pb;
    if(!PyArg_ParseTuple(args, "kKk|K", &dwPID, &qwA, &cb, &flags)) { return NULL; }
    if(cb > 0x01000000) { return PyErr_Format(PyExc_RuntimeError, "VMMPYC_MemRead: Read larger than maximum supported (0x01000000) bytes requested."); }
    pb = LocalAlloc(0, cb);
    if(!pb) { return PyErr_NoMemory(); }
    Py_BEGIN_ALLOW_THREADS;
    result = VMMDLL_MemReadEx(dwPID, qwA, pb, cb, &cbRead, flags);
    Py_END_ALLOW_THREADS;
    if(!result) { 
        LocalFree(pb);
        return PyErr_Format(PyExc_RuntimeError, "VMMPYC_MemRead: Failed.");
    }
    pyBytes = PyBytes_FromStringAndSize(pb, cbRead);
    LocalFree(pb);
    return pyBytes;
}

// (DWORD, ULONG64, PBYTE) -> None
static PyObject*
VMMPYC_MemWrite(PyObject *self, PyObject *args)
{
    BOOL result;
    DWORD dwPID;
    ULONG64 va;
    PBYTE pb, pbPy;
    DWORD cb;
    if(!PyArg_ParseTuple(args, "kKy#", &dwPID, &va, &pbPy, &cb)) { return NULL; }
    if(cb == 0) {
        return Py_BuildValue("s", NULL);    // zero-byte write is always successful.
    }
    pb = LocalAlloc(0, cb);
    if(!pb) {
        return PyErr_NoMemory();
    }
    memcpy(pb, pbPy, cb);
    Py_BEGIN_ALLOW_THREADS;
    result = VMMDLL_MemWrite(dwPID, va, pb, (DWORD)cb);
    LocalFree(pb);
    Py_END_ALLOW_THREADS;
    if(!result) { return PyErr_Format(PyExc_RuntimeError, "VMMPYC_MemWrite: Failed."); }
    return Py_BuildValue("s", NULL);    // None returned on success.
}

// (DWORD, ULONG64) -> ULONG64
static PyObject*
VMMPYC_MemVirt2Phys(PyObject *self, PyObject *args)
{
    BOOL result;
    DWORD dwPID;
    ULONG64 va, pa;
    if(!PyArg_ParseTuple(args, "kK", &dwPID, &va)) { return NULL; }
    Py_BEGIN_ALLOW_THREADS;
    result = VMMDLL_MemVirt2Phys(dwPID, va, &pa);
    Py_END_ALLOW_THREADS;
    if(!result) { return PyErr_Format(PyExc_RuntimeError, "VMMPYC_MemVirt2Phys: Failed."); }
    return PyLong_FromUnsignedLongLong(pa);
}

// (DWORD, (BOOL)) -> [{...}]
static PyObject*
VMMPYC_ProcessGetPteMap(PyObject *self, PyObject *args)
{
    PyObject *pyList, *pyDict;
    BOOL result, fIdentifyModules;
    DWORD dwPID, i;
    DWORD cbPteMap = 0;
    PVMMDLL_MAP_PTEENTRY pe;
    PVMMDLL_MAP_PTE pPteMap = NULL;
    CHAR sz[5];
    if(!PyArg_ParseTuple(args, "k|p", &dwPID, &fIdentifyModules)) { return NULL; }
    if(!(pyList = PyList_New(0))) { return PyErr_NoMemory(); }
    Py_BEGIN_ALLOW_THREADS;
    result =
        VMMDLL_ProcessMap_GetPte(dwPID, NULL, &cbPteMap, fIdentifyModules) &&
        cbPteMap &&
        (pPteMap = LocalAlloc(0, cbPteMap)) &&
        VMMDLL_ProcessMap_GetPte(dwPID, pPteMap, &cbPteMap, fIdentifyModules);
    Py_END_ALLOW_THREADS;
    if(!result) { 
        Py_DECREF(pyList);
        LocalFree(pPteMap);
        return PyErr_Format(PyExc_RuntimeError, "VMMPYC_ProcessGetPteMap: Failed.");
    }
    for(i = 0; i < pPteMap->cMap; i++) {
        if((pyDict = PyDict_New())) {
            pe = pPteMap->pMap + i;
            PyDict_SetItemString_DECREF(pyDict, "va", PyLong_FromUnsignedLongLong(pe->vaBase));
            PyDict_SetItemString_DECREF(pyDict, "size", PyLong_FromUnsignedLongLong(pe->cPages << 12));
            PyDict_SetItemString_DECREF(pyDict, "pages", PyLong_FromUnsignedLongLong(pe->cPages));
            PyDict_SetItemString_DECREF(pyDict, "wow64", PyBool_FromLong((long)pe->fWoW64));
            PyDict_SetItemString_DECREF(pyDict, "tag", PyUnicode_FromWideChar(pe->wszText, -1));
            PyDict_SetItemString_DECREF(pyDict, "flags-pte", PyLong_FromUnsignedLongLong(pe->fPage));
            sz[0] = (pe->fPage & VMMDLL_MEMMAP_FLAG_PAGE_NS) ? '-' : 's';
            sz[1] = 'r';
            sz[2] = (pe->fPage & VMMDLL_MEMMAP_FLAG_PAGE_W) ? 'w' : '-';
            sz[3] = (pe->fPage & VMMDLL_MEMMAP_FLAG_PAGE_NX) ? '-' : 'x';
            sz[4] = 0;
            PyDict_SetItemString_DECREF(pyDict, "flags", PyUnicode_FromFormat("%s", sz));
            PyList_Append_DECREF(pyList, pyDict);
        }
    }
    LocalFree(pPteMap);
    return pyList;
}

VOID VMMPYC_ProcessGetVadMap_Protection(_In_ PVMMDLL_MAP_VADENTRY pVad, _Out_writes_(6) LPSTR sz)
{
    BYTE vh = (BYTE)pVad->Protection >> 3;
    BYTE vl = (BYTE)pVad->Protection & 7;
    sz[0] = pVad->fPrivateMemory ? 'p' : '-';                                    // PRIVATE MEMORY
    sz[1] = (vh & 2) ? ((vh & 1) ? 'm' : 'g') : ((vh & 1) ? 'n' : '-');         // -/NO_CACHE/GUARD/WRITECOMBINE
    sz[2] = ((vl == 1) || (vl == 3) || (vl == 4) || (vl == 6)) ? 'r' : '-';     // COPY ON WRITE
    sz[3] = (vl & 4) ? 'w' : '-';                                               // WRITE
    sz[4] = (vl & 2) ? 'x' : '-';                                               // EXECUTE
    sz[5] = ((vl == 5) || (vl == 7)) ? 'c' : '-';                               // COPY ON WRITE
    if(sz[1] != '-' && sz[2] == '-' && sz[3] == '-' && sz[4] == '-' && sz[5] == '-') { sz[1] = '-'; }
}

LPSTR VMMPYC_ProcessGetVadMap_Type(_In_ PVMMDLL_MAP_VADENTRY pVad)
{
    if(pVad->fImage) {
        return "Image";
    } else if(pVad->fFile) {
        return "File ";
    } else if(pVad->fHeap) {
        return "Heap ";
    } else if(pVad->fStack) {
        return "Stack";
    } else if(pVad->fTeb) {
        return "Teb  ";
    } else if(pVad->fPageFile) {
        return "Pf   ";
    } else {
        return "     ";
    }
}

// (DWORD, (BOOL)) -> [{...}]
static PyObject*
VMMPYC_ProcessGetVadMap(PyObject *self, PyObject *args)
{
    PyObject *pyList, *pyDict;
    BOOL result, fIdentifyModules;
    DWORD dwPID, i;
    DWORD cbVadMap = 0;
    PVMMDLL_MAP_VADENTRY pe;
    PVMMDLL_MAP_VAD pVadMap = NULL;
    CHAR szVadProtection[7] = { 0 };
    if(!PyArg_ParseTuple(args, "k|p", &dwPID, &fIdentifyModules)) { return NULL; }
    if(!(pyList = PyList_New(0))) { return PyErr_NoMemory(); }
    Py_BEGIN_ALLOW_THREADS;
    result =
        VMMDLL_ProcessMap_GetVad(dwPID, NULL, &cbVadMap, fIdentifyModules) &&
        cbVadMap &&
        (pVadMap = LocalAlloc(0, cbVadMap)) &&
        VMMDLL_ProcessMap_GetVad(dwPID, pVadMap, &cbVadMap, fIdentifyModules);
    Py_END_ALLOW_THREADS;
    if(!result) {
        Py_DECREF(pyList);
        LocalFree(pVadMap);
        return PyErr_Format(PyExc_RuntimeError, "VMMPYC_ProcessGetVadMap: Failed.");
    }
    for(i = 0; i < pVadMap->cMap; i++) {
        if((pyDict = PyDict_New())) {
            pe = pVadMap->pMap + i;
            VMMPYC_ProcessGetVadMap_Protection(pe, szVadProtection);
            PyDict_SetItemString_DECREF(pyDict, "start", PyLong_FromUnsignedLongLong(pe->vaStart));
            PyDict_SetItemString_DECREF(pyDict, "end", PyLong_FromUnsignedLongLong(pe->vaEnd));
            PyDict_SetItemString_DECREF(pyDict, "subsection", PyLong_FromUnsignedLongLong(pe->vaSubsection));
            PyDict_SetItemString_DECREF(pyDict, "prototype", PyLong_FromUnsignedLongLong(pe->vaPrototypePte));
            PyDict_SetItemString_DECREF(pyDict, "prototype-len", PyLong_FromUnsignedLong(pe->cbPrototypePte));
            PyDict_SetItemString_DECREF(pyDict, "mem_commit", PyBool_FromLong((long)pe->MemCommit));
            PyDict_SetItemString_DECREF(pyDict, "commit_charge", PyLong_FromUnsignedLong(pe->CommitCharge));
            PyDict_SetItemString_DECREF(pyDict, "protection", PyUnicode_FromFormat("%s", szVadProtection));
            PyDict_SetItemString_DECREF(pyDict, "type", PyUnicode_FromFormat("%s", VMMPYC_ProcessGetVadMap_Type(pe)));
            PyDict_SetItemString_DECREF(pyDict, "tag", PyUnicode_FromWideChar(pe->wszText, -1));
            PyList_Append_DECREF(pyList, pyDict);
        }
    }
    LocalFree(pVadMap);
    return pyList;
}

// (DWORD) -> [{...}]
static PyObject*
VMMPYC_ProcessGetModuleMap(PyObject *self, PyObject *args)
{
    PyObject *pyList, *pyDict;
    BOOL result;
    DWORD dwPID, cbModuleMap = 0;
    ULONG64 i;
    PVMMDLL_MAP_MODULE pModuleMap = NULL;
    PVMMDLL_MAP_MODULEENTRY pe;
    if(!PyArg_ParseTuple(args, "k", &dwPID)) { return NULL; }
    if(!(pyList = PyList_New(0))) { return PyErr_NoMemory(); }
    Py_BEGIN_ALLOW_THREADS;
    result =
        VMMDLL_ProcessMap_GetModule(dwPID, NULL, &cbModuleMap) &&
        cbModuleMap &&
        (pModuleMap = LocalAlloc(0, cbModuleMap)) &&
        VMMDLL_ProcessMap_GetModule(dwPID, pModuleMap, &cbModuleMap);
    Py_END_ALLOW_THREADS;
    if(!result) {
        Py_DECREF(pyList);
        LocalFree(pModuleMap);
        return PyErr_Format(PyExc_RuntimeError, "VMMPYC_ProcessGetModuleMap: Failed.");
    }
    for(i = 0; i < pModuleMap->cMap; i++) {
        if((pyDict = PyDict_New())) {
            pe = pModuleMap->pMap + i;
            PyDict_SetItemString_DECREF(pyDict, "va", PyLong_FromUnsignedLongLong(pe->vaBase));
            PyDict_SetItemString_DECREF(pyDict, "va-entry", PyLong_FromUnsignedLongLong(pe->vaEntry));
            PyDict_SetItemString_DECREF(pyDict, "size", PyLong_FromUnsignedLong(pe->cbImageSize));
            PyDict_SetItemString_DECREF(pyDict, "wow64", PyBool_FromLong((long)pe->fWoW64));
            PyDict_SetItemString_DECREF(pyDict, "name", PyUnicode_FromWideChar(pe->wszText, -1));
            PyList_Append_DECREF(pyList, pyDict);
        }
    }
    LocalFree(pModuleMap);
    return pyList;
}

// (DWORD, STR) -> {...}
static PyObject*
VMMPYC_ProcessGetModuleFromName(PyObject *self, PyObject *args)
{
    PyObject *pyDict, *pyUnicodePath;
    BOOL result;
    DWORD dwPID;
    LPWSTR wszModuleName = NULL;
    VMMDLL_MAP_MODULEENTRY e;
    if(!PyArg_ParseTuple(args, "kO!", &dwPID, &PyUnicode_Type, &pyUnicodePath)) { return NULL; }
    if(!(pyDict = PyDict_New())) { return PyErr_NoMemory(); }
    if(!(wszModuleName = PyUnicode_AsWideCharString(pyUnicodePath, NULL))) { return NULL; }     // wszPath PyMem_Free() required 
    Py_BEGIN_ALLOW_THREADS;
    ZeroMemory(&e, sizeof(VMMDLL_MAP_MODULEENTRY));
    result = VMMDLL_ProcessMap_GetModuleFromName(dwPID, wszModuleName, &e);
    Py_END_ALLOW_THREADS;
    PyMem_Free(wszModuleName);
    if(!result) { 
        Py_DECREF(pyDict);
        return PyErr_Format(PyExc_RuntimeError, "VMMPYC_ProcessGetModuleFromName: Failed.");
    }
    PyDict_SetItemString_DECREF(pyDict, "va", PyLong_FromUnsignedLongLong(e.vaBase));
    PyDict_SetItemString_DECREF(pyDict, "va-entry", PyLong_FromUnsignedLongLong(e.vaEntry));
    PyDict_SetItemString_DECREF(pyDict, "wow64", PyBool_FromLong((long)e.fWoW64));
    PyDict_SetItemString_DECREF(pyDict, "size", PyLong_FromUnsignedLong(e.cbImageSize));
    PyDict_SetItemString(pyDict, "name", pyUnicodePath);
    return pyDict;
}


// (DWORD) -> [{...}]
static PyObject*
VMMPYC_ProcessGetHeapMap(PyObject *self, PyObject *args)
{
    PyObject *pyList, *pyDict;
    BOOL result;
    DWORD dwPID, i;
    DWORD cbHeapMap = 0;
    PVMMDLL_MAP_HEAPENTRY pe;
    PVMMDLL_MAP_HEAP pHeapMap = NULL;
    if(!PyArg_ParseTuple(args, "k", &dwPID)) { return NULL; }
    if(!(pyList = PyList_New(0))) { return PyErr_NoMemory(); }
    Py_BEGIN_ALLOW_THREADS;
    result =
        VMMDLL_ProcessMap_GetHeap(dwPID, NULL, &cbHeapMap) &&
        cbHeapMap &&
        (pHeapMap = LocalAlloc(0, cbHeapMap)) &&
        VMMDLL_ProcessMap_GetHeap(dwPID, pHeapMap, &cbHeapMap);
    Py_END_ALLOW_THREADS;
    if(!result) {
        Py_DECREF(pyList);
        LocalFree(pHeapMap);
        return PyErr_Format(PyExc_RuntimeError, "VMMPYC_ProcessGetHeapMap: Failed.");
    }
    for(i = 0; i < pHeapMap->cMap; i++) {
        if((pyDict = PyDict_New())) {
            pe = pHeapMap->pMap + i;
            PyDict_SetItemString_DECREF(pyDict, "va", PyLong_FromUnsignedLongLong(pe->vaHeapSegment));
            PyDict_SetItemString_DECREF(pyDict, "size", PyLong_FromUnsignedLong(pe->cPages << 12));
            PyDict_SetItemString_DECREF(pyDict, "size-uncommitted", PyLong_FromUnsignedLong(pe->cPagesUnCommitted << 12));
            PyDict_SetItemString_DECREF(pyDict, "id", PyLong_FromUnsignedLong(pe->HeapId));
            PyDict_SetItemString_DECREF(pyDict, "primary", PyBool_FromLong((long)pe->fPrimary));
            PyList_Append_DECREF(pyList, pyDict);
        }
    }
    LocalFree(pHeapMap);
    return pyList;
}

// (DWORD) -> [{...}]
static PyObject*
VMMPYC_ProcessGetThreadMap(PyObject *self, PyObject *args)
{
    PyObject *pyList, *pyDict;
    BOOL result;
    DWORD dwPID, i;
    DWORD cbThreadMap = 0;
    PVMMDLL_MAP_THREADENTRY pe;
    PVMMDLL_MAP_THREAD pThreadMap = NULL;
    CHAR szTimeUTC[MAX_PATH];
    if(!PyArg_ParseTuple(args, "k", &dwPID)) { return NULL; }
    if(!(pyList = PyList_New(0))) { return PyErr_NoMemory(); }
    Py_BEGIN_ALLOW_THREADS;
    result =
        VMMDLL_ProcessMap_GetThread(dwPID, NULL, &cbThreadMap) &&
        cbThreadMap &&
        (pThreadMap = LocalAlloc(0, cbThreadMap)) &&
        VMMDLL_ProcessMap_GetThread(dwPID, pThreadMap, &cbThreadMap);
    Py_END_ALLOW_THREADS;
    if(!result) {
        Py_DECREF(pyList);
        LocalFree(pThreadMap);
        return PyErr_Format(PyExc_RuntimeError, "VMMPYC_ProcessGetThreadMap: Failed.");
    }
    for(i = 0; i < pThreadMap->cMap; i++) {
        if((pyDict = PyDict_New())) {
            pe = pThreadMap->pMap + i;
            PyDict_SetItemString_DECREF(pyDict, "tid", PyLong_FromUnsignedLong(pe->dwTID));
            PyDict_SetItemString_DECREF(pyDict, "pid", PyLong_FromUnsignedLong(pe->dwPID));
            PyDict_SetItemString_DECREF(pyDict, "exitstatus", PyLong_FromUnsignedLong(pe->dwExitStatus));
            PyDict_SetItemString_DECREF(pyDict, "state", PyLong_FromUnsignedLong(pe->bState));
            PyDict_SetItemString_DECREF(pyDict, "running", PyLong_FromUnsignedLong(pe->bRunning));
            PyDict_SetItemString_DECREF(pyDict, "priority", PyLong_FromUnsignedLong(pe->bPriority));
            PyDict_SetItemString_DECREF(pyDict, "basepriority", PyLong_FromUnsignedLong(pe->bBasePriority));
            PyDict_SetItemString_DECREF(pyDict, "va-ethread", PyLong_FromUnsignedLongLong(pe->vaETHREAD));
            PyDict_SetItemString_DECREF(pyDict, "va-teb", PyLong_FromUnsignedLongLong(pe->vaTeb));
            PyDict_SetItemString_DECREF(pyDict, "va-start", PyLong_FromUnsignedLongLong(pe->vaStartAddress));
            PyDict_SetItemString_DECREF(pyDict, "va-stackbase", PyLong_FromUnsignedLongLong(pe->vaStackBaseUser));
            PyDict_SetItemString_DECREF(pyDict, "va-stacklimit", PyLong_FromUnsignedLongLong(pe->vaStackLimitUser));
            PyDict_SetItemString_DECREF(pyDict, "va-stackbase-kernel", PyLong_FromUnsignedLongLong(pe->vaStackBaseKernel));
            PyDict_SetItemString_DECREF(pyDict, "va-stacklimit-kernel", PyLong_FromUnsignedLongLong(pe->vaStackLimitKernel));
            PyDict_SetItemString_DECREF(pyDict, "time-create", PyLong_FromUnsignedLongLong(pe->ftCreateTime));
            PyDict_SetItemString_DECREF(pyDict, "time-exit", PyLong_FromUnsignedLongLong(pe->ftExitTime));
            Util_FileTime2String((PFILETIME)&pe->ftCreateTime, szTimeUTC);
            PyDict_SetItemString_DECREF(pyDict, "time-create-str", PyUnicode_FromFormat("%s", szTimeUTC));
            Util_FileTime2String((PFILETIME)&pe->ftExitTime, szTimeUTC);
            PyDict_SetItemString_DECREF(pyDict, "time-exit-str", PyUnicode_FromFormat("%s", szTimeUTC));
            PyList_Append_DECREF(pyList, pyDict);
        }
    }
    LocalFree(pThreadMap);
    return pyList;
}

// (DWORD) -> [{...}]
static PyObject*
VMMPYC_ProcessGetHandleMap(PyObject *self, PyObject *args)
{
    PyObject *pyList, *pyDict;
    BOOL result;
    DWORD dwPID, cbHandleMap = 0;
    ULONG64 i;
    PVMMDLL_MAP_HANDLE pHandleMap = NULL;
    PVMMDLL_MAP_HANDLEENTRY pe;
    if(!PyArg_ParseTuple(args, "k", &dwPID)) { return NULL; }
    if(!(pyList = PyList_New(0))) { return PyErr_NoMemory(); }
    Py_BEGIN_ALLOW_THREADS;
    result =
        VMMDLL_ProcessMap_GetHandle(dwPID, NULL, &cbHandleMap) &&
        cbHandleMap &&
        (pHandleMap = LocalAlloc(0, cbHandleMap)) &&
        VMMDLL_ProcessMap_GetHandle(dwPID, pHandleMap, &cbHandleMap);
    Py_END_ALLOW_THREADS;
    if(!result) {
        Py_DECREF(pyList);
        LocalFree(pHandleMap);
        return PyErr_Format(PyExc_RuntimeError, "VMMPYC_ProcessGetHandleMap: Failed.");
    }
    for(i = 0; i < pHandleMap->cMap; i++) {
        if((pyDict = PyDict_New())) {
            pe = pHandleMap->pMap + i;
            PyDict_SetItemString_DECREF(pyDict, "va-object", PyLong_FromUnsignedLongLong(pe->vaObject));
            PyDict_SetItemString_DECREF(pyDict, "handle", PyLong_FromUnsignedLong(pe->dwHandle));
            PyDict_SetItemString_DECREF(pyDict, "access", PyLong_FromUnsignedLong(pe->dwGrantedAccess));
            PyDict_SetItemString_DECREF(pyDict, "typeindex", PyLong_FromUnsignedLong(pe->iType));
            PyDict_SetItemString_DECREF(pyDict, "pid", PyLong_FromUnsignedLong(pe->dwPID));
            PyDict_SetItemString_DECREF(pyDict, "pooltag", PyLong_FromUnsignedLong(pe->dwPoolTag));
            PyDict_SetItemString_DECREF(pyDict, "chandle", PyLong_FromUnsignedLongLong(pe->qwHandleCount));
            PyDict_SetItemString_DECREF(pyDict, "cpointer", PyLong_FromUnsignedLongLong(pe->qwPointerCount));
            PyDict_SetItemString_DECREF(pyDict, "va-object-creatinfo", PyLong_FromUnsignedLongLong(pe->vaObjectCreateInfo));
            PyDict_SetItemString_DECREF(pyDict, "va-securitydescriptor", PyLong_FromUnsignedLongLong(pe->vaSecurityDescriptor));
            PyDict_SetItemString_DECREF(pyDict, "tag", PyUnicode_FromWideChar(pe->wszText, -1));
            PyDict_SetItemString_DECREF(pyDict, "type", PyUnicode_FromWideChar(pe->wszType, -1));
            PyList_Append_DECREF(pyList, pyDict);
        }
    }
    LocalFree(pHandleMap);
    return pyList;
}

// (STR) -> DWORD
static PyObject*
VMMPYC_PidGetFromName(PyObject *self, PyObject *args)
{
    BOOL result;
    DWORD dwPID;
    LPSTR szProcessName;
    if(!PyArg_ParseTuple(args, "s", &szProcessName)) { return NULL; }
    Py_BEGIN_ALLOW_THREADS;
    result = VMMDLL_PidGetFromName(szProcessName, &dwPID);
    Py_END_ALLOW_THREADS;
    if(!result) { return PyErr_Format(PyExc_RuntimeError, "VMMPYC_PidGetFromName: Failed."); }
    return PyLong_FromLong(dwPID);
}

// () -> [DWORD]
static PyObject*
VMMPYC_PidList(PyObject *self, PyObject *args)
{
    PyObject *pyList;
    BOOL result;
    ULONG64 cPIDs = 0;
    DWORD i, *pPIDs = NULL;
    if(!(pyList = PyList_New(0))) { return PyErr_NoMemory(); }
    Py_BEGIN_ALLOW_THREADS;
    result =
        VMMDLL_PidList(NULL, &cPIDs) &&
        (pPIDs = LocalAlloc(LMEM_ZEROINIT, cPIDs * sizeof(DWORD))) &&
        VMMDLL_PidList(pPIDs, &cPIDs);
    Py_END_ALLOW_THREADS;
    if(!result) {
        Py_DECREF(pyList);
        LocalFree(pPIDs);
        return PyErr_Format(PyExc_RuntimeError, "VMMPYC_PidList: Failed.");
    }
    for(i = 0; i < cPIDs; i++) {
        PyList_Append_DECREF(pyList, PyLong_FromUnsignedLong(pPIDs[i]));
    }
    LocalFree(pPIDs);
    return pyList;
}

// (DWORD) -> {...}
static PyObject*
VMMPYC_ProcessGetInformation(PyObject *self, PyObject *args)
{
    PyObject *pyDict;
    BOOL result;
    DWORD dwPID;
    VMMDLL_PROCESS_INFORMATION info;
    SIZE_T cbInfo = sizeof(VMMDLL_PROCESS_INFORMATION);
    LPSTR szPathKernel = NULL, szPathUser = NULL, szCmdLine = NULL;
    if(!PyArg_ParseTuple(args, "k", &dwPID)) { return NULL; }
    if(!(pyDict = PyDict_New())) { return PyErr_NoMemory(); }
    Py_BEGIN_ALLOW_THREADS;
    ZeroMemory(&info, sizeof(VMMDLL_PROCESS_INFORMATION));
    info.magic = VMMDLL_PROCESS_INFORMATION_MAGIC;
    info.wVersion = VMMDLL_PROCESS_INFORMATION_VERSION;
    result = VMMDLL_ProcessGetInformation(dwPID, &info, &cbInfo);
    if(result) {
        szPathKernel = VMMDLL_ProcessGetInformationString(dwPID, VMMDLL_PROCESS_INFORMATION_OPT_STRING_PATH_KERNEL);
        szPathUser = VMMDLL_ProcessGetInformationString(dwPID, VMMDLL_PROCESS_INFORMATION_OPT_STRING_PATH_USER_IMAGE);
        szCmdLine = VMMDLL_ProcessGetInformationString(dwPID, VMMDLL_PROCESS_INFORMATION_OPT_STRING_CMDLINE);
    }
    Py_END_ALLOW_THREADS;
    if(!result) {
        Py_DECREF(pyDict);
        return PyErr_Format(PyExc_RuntimeError, "VMMPYC_ProcessGetInformation: Failed.");
    }
    PyDict_SetItemString_DECREF(pyDict, "pid", PyLong_FromUnsignedLong(info.dwPID));
    PyDict_SetItemString_DECREF(pyDict, "ppid", PyLong_FromUnsignedLong(info.dwPPID));
    PyDict_SetItemString_DECREF(pyDict, "pa-dtb", PyLong_FromUnsignedLongLong(info.paDTB));
    PyDict_SetItemString_DECREF(pyDict, "pa-dtb-user", PyLong_FromUnsignedLongLong(info.paDTB_UserOpt));
    PyDict_SetItemString_DECREF(pyDict, "state", PyLong_FromUnsignedLong(info.dwState));
    PyDict_SetItemString_DECREF(pyDict, "tp-memorymodel", PyLong_FromUnsignedLong(info.tpMemoryModel));
    PyDict_SetItemString_DECREF(pyDict, "tp-system", PyLong_FromUnsignedLong(info.tpSystem));
    PyDict_SetItemString_DECREF(pyDict, "usermode", PyBool_FromLong(info.fUserOnly));
    PyDict_SetItemString_DECREF(pyDict, "name", PyUnicode_FromFormat("%s", info.szName));
    PyDict_SetItemString_DECREF(pyDict, "name-long", PyUnicode_FromFormat("%s", info.szNameLong));
    PyDict_SetItemString_DECREF(pyDict, "path-kernel", PyUnicode_FromFormat("%s", szPathKernel ? szPathKernel : ""));
    PyDict_SetItemString_DECREF(pyDict, "path-user", PyUnicode_FromFormat("%s", szPathUser ? szPathUser : ""));
    PyDict_SetItemString_DECREF(pyDict, "cmdline", PyUnicode_FromFormat("%s", szCmdLine ? szCmdLine : ""));
    switch(info.tpSystem) {
        case VMMDLL_SYSTEM_WINDOWS_X64:
            PyDict_SetItemString_DECREF(pyDict, "wow64", PyBool_FromLong((long)info.os.win.fWow64));
            PyDict_SetItemString_DECREF(pyDict, "va-eprocess", PyLong_FromUnsignedLongLong(info.os.win.vaEPROCESS));
            PyDict_SetItemString_DECREF(pyDict, "va-peb", PyLong_FromUnsignedLongLong(info.os.win.vaPEB));
            PyDict_SetItemString_DECREF(pyDict, "va-peb32", PyLong_FromUnsignedLongLong(info.os.win.vaPEB32));
            break;
        case VMMDLL_SYSTEM_WINDOWS_X86:
            PyDict_SetItemString_DECREF(pyDict, "va-eprocess", PyLong_FromUnsignedLongLong(info.os.win.vaEPROCESS));
            PyDict_SetItemString_DECREF(pyDict, "va-peb", PyLong_FromUnsignedLongLong(info.os.win.vaPEB));
            break;
    }
    VMMDLL_MemFree(szPathKernel);
    VMMDLL_MemFree(szPathUser);
    VMMDLL_MemFree(szCmdLine);
    return pyDict;
}

// (DWORD, STR) -> [{...}]
static PyObject*
VMMPYC_ProcessGetDirectories(PyObject *self, PyObject *args)
{
    PyObject *pyList, *pyDict, *pyUnicodeModule;
    BOOL result;
    DWORD i, dwPID, cDirectories;
    PIMAGE_DATA_DIRECTORY pe, pDirectories = NULL;
    LPWSTR wszModule = NULL;
    LPCSTR DIRECTORIES[16] = { "EXPORT", "IMPORT", "RESOURCE", "EXCEPTION", "SECURITY", "BASERELOC", "DEBUG", "ARCHITECTURE", "GLOBALPTR", "TLS", "LOAD_CONFIG", "BOUND_IMPORT", "IAT", "DELAY_IMPORT", "COM_DESCRIPTOR", "RESERVED" };
    if(!PyArg_ParseTuple(args, "kO!", &dwPID, &PyUnicode_Type, &pyUnicodeModule)) { return NULL; }
    if(!(pyList = PyList_New(0))) { return PyErr_NoMemory(); }
    if(!(wszModule = PyUnicode_AsWideCharString(pyUnicodeModule, NULL))) { return NULL; }       // wszPath PyMem_Free() required
    Py_BEGIN_ALLOW_THREADS;
    result =
        (pDirectories = LocalAlloc(0, 16 * sizeof(IMAGE_DATA_DIRECTORY))) &&
        VMMDLL_ProcessGetDirectories(dwPID, wszModule, pDirectories, 16, &cDirectories);
    Py_END_ALLOW_THREADS;
    PyMem_Free(wszModule);
    if(!result) {
        Py_DECREF(pyList);
        LocalFree(pDirectories);
        return PyErr_Format(PyExc_RuntimeError, "VMMPYC_ProcessGetDirectories: Failed.");
    }
    for(i = 0; i < 16; i++) {
        if((pyDict = PyDict_New())) {
            pe = pDirectories + i;
            PyDict_SetItemString_DECREF(pyDict, "i", PyLong_FromUnsignedLong(i));
            PyDict_SetItemString_DECREF(pyDict, "size", PyLong_FromUnsignedLong(pe->Size));
            PyDict_SetItemString_DECREF(pyDict, "offset", PyLong_FromUnsignedLong(pe->VirtualAddress));
            PyDict_SetItemString_DECREF(pyDict, "name", PyUnicode_FromFormat("%s", DIRECTORIES[i]));
            PyList_Append_DECREF(pyList, pyDict);
        }
    }
    LocalFree(pDirectories);
    return pyList;
}

// (DWORD, STR) -> [{...}]
static PyObject*
VMMPYC_ProcessGetSections(PyObject *self, PyObject *args)
{
    PyObject *pyList, *pyDict, *pyUnicodeModule;
    BOOL result;
    DWORD i, dwPID, cSections;
    PIMAGE_SECTION_HEADER pe, pSections = NULL;
    LPWSTR wszModule = NULL;
    CHAR szName[9];
    szName[8] = 0;
    if(!PyArg_ParseTuple(args, "kO!", &dwPID, &PyUnicode_Type, &pyUnicodeModule)) { return NULL; }
    if(!(pyList = PyList_New(0))) { return PyErr_NoMemory(); }
    if(!(wszModule = PyUnicode_AsWideCharString(pyUnicodeModule, NULL))) { return NULL; }       // wszPath PyMem_Free() required
    Py_BEGIN_ALLOW_THREADS;
    result =
        VMMDLL_ProcessGetSections(dwPID, wszModule, NULL, 0, &cSections) &&
        cSections &&
        (pSections = LocalAlloc(0, cSections * sizeof(IMAGE_SECTION_HEADER))) &&
        VMMDLL_ProcessGetSections(dwPID, wszModule, pSections, cSections, &cSections);
    Py_END_ALLOW_THREADS;
    PyMem_Free(wszModule);
    if(!result) {
        Py_DECREF(pyList);
        LocalFree(pSections);
        return PyErr_Format(PyExc_RuntimeError, "VMMPYC_ProcessGetSections: Failed.");
    }
    for(i = 0; i < cSections; i++) {
        if((pyDict = PyDict_New())) {
            pe = pSections + i;
            PyDict_SetItemString_DECREF(pyDict, "i", PyLong_FromUnsignedLong(i));
            PyDict_SetItemString_DECREF(pyDict, "Characteristics", PyLong_FromUnsignedLong(pe->Characteristics));
            PyDict_SetItemString_DECREF(pyDict, "misc-PhysicalAddress", PyLong_FromUnsignedLong(pe->Misc.PhysicalAddress));
            PyDict_SetItemString_DECREF(pyDict, "misc-VirtualSize", PyLong_FromUnsignedLong(pe->Misc.VirtualSize));
            *(PULONG64)szName = *(PULONG64)pe->Name;
            PyDict_SetItemString(pyDict, "Name", pyUnicodeModule);
            PyDict_SetItemString_DECREF(pyDict, "NumberOfLinenumbers", PyLong_FromUnsignedLong(pe->NumberOfLinenumbers));
            PyDict_SetItemString_DECREF(pyDict, "NumberOfRelocations", PyLong_FromUnsignedLong(pe->NumberOfRelocations));
            PyDict_SetItemString_DECREF(pyDict, "PointerToLinenumbers", PyLong_FromUnsignedLong(pe->PointerToLinenumbers));
            PyDict_SetItemString_DECREF(pyDict, "PointerToRawData", PyLong_FromUnsignedLong(pe->PointerToRawData));
            PyDict_SetItemString_DECREF(pyDict, "PointerToRelocations", PyLong_FromUnsignedLong(pe->PointerToRelocations));
            PyDict_SetItemString_DECREF(pyDict, "SizeOfRawData", PyLong_FromUnsignedLong(pe->SizeOfRawData));
            PyDict_SetItemString_DECREF(pyDict, "VirtualAddress", PyLong_FromUnsignedLong(pe->VirtualAddress));
            PyList_Append_DECREF(pyList, pyDict);
        }
    }
    LocalFree(pSections);
    return pyList;
}

// (DWORD, STR) -> [{...}]
static PyObject*
VMMPYC_ProcessGetEAT(PyObject *self, PyObject *args)
{
    PyObject *pyList, *pyDict, *pyUnicodeModule;
    BOOL result;
    DWORD i, dwPID, cEATs;
    PVMMDLL_EAT_ENTRY pe, pEATs = NULL;
    LPWSTR wszModule = NULL;
    if(!PyArg_ParseTuple(args, "kO!", &dwPID, &PyUnicode_Type, &pyUnicodeModule)) { return NULL; }
    if(!(pyList = PyList_New(0))) { return PyErr_NoMemory(); }
    if(!(wszModule = PyUnicode_AsWideCharString(pyUnicodeModule, NULL))) { return NULL; }       // wszPath PyMem_Free() required
    Py_BEGIN_ALLOW_THREADS;
    result =
        VMMDLL_ProcessGetEAT(dwPID, wszModule, NULL, 0, &cEATs) &&
        cEATs &&
        (pEATs = LocalAlloc(0, cEATs * sizeof(VMMDLL_EAT_ENTRY))) &&
        VMMDLL_ProcessGetEAT(dwPID, wszModule, pEATs, cEATs, &cEATs);
    Py_END_ALLOW_THREADS;
    PyMem_Free(wszModule);
    if(!result) {
        Py_DECREF(pyList);
        LocalFree(pEATs);
        return PyErr_Format(PyExc_RuntimeError, "VMMPYC_ProcessGetEAT: Failed.");
    }
    for(i = 0; i < cEATs; i++) {
        if((pyDict = PyDict_New())) {
            pe = pEATs + i;
            PyDict_SetItemString_DECREF(pyDict, "i", PyLong_FromUnsignedLong(i));
            PyDict_SetItemString_DECREF(pyDict, "va", PyLong_FromUnsignedLongLong(pe->vaFunction));
            PyDict_SetItemString_DECREF(pyDict, "offset", PyLong_FromUnsignedLong(pe->vaFunctionOffset));
            PyDict_SetItemString_DECREF(pyDict, "fn", PyUnicode_FromFormat("%s", pe->szFunction));
            PyList_Append_DECREF(pyList, pyDict);
        }
    }
    LocalFree(pEATs);
    return pyList;
}

// (DWORD, STR) -> [{...}]
static PyObject*
VMMPYC_ProcessGetIAT(PyObject *self, PyObject *args)
{
    PyObject *pyList, *pyDict, *pyUnicodeModule;
    BOOL result;
    DWORD i, dwPID, cIATs;
    PVMMDLL_IAT_ENTRY pe, pIATs = NULL;
    LPWSTR wszModule = NULL;
    if(!PyArg_ParseTuple(args, "kO!", &dwPID, &PyUnicode_Type, &pyUnicodeModule)) { return NULL; }
    if(!(pyList = PyList_New(0))) { return PyErr_NoMemory(); }
    if(!(wszModule = PyUnicode_AsWideCharString(pyUnicodeModule, NULL))) { return NULL; }       // wszPath PyMem_Free() required
    Py_BEGIN_ALLOW_THREADS;
    result =
        VMMDLL_ProcessGetIAT(dwPID, wszModule, NULL, 0, &cIATs) &&
        cIATs &&
        (pIATs = LocalAlloc(0, cIATs * sizeof(VMMDLL_IAT_ENTRY))) &&
        VMMDLL_ProcessGetIAT(dwPID, wszModule, pIATs, cIATs, &cIATs);
    Py_END_ALLOW_THREADS;
    PyMem_Free(wszModule);
    if(!result) {
        Py_DECREF(pyList);
        LocalFree(pIATs);
        return PyErr_Format(PyExc_RuntimeError, "VMMPYC_ProcessGetIAT: Failed.");
    }
    for(i = 0; i < cIATs; i++) {
        if((pyDict = PyDict_New())) {
            pe = pIATs + i;
            PyDict_SetItemString_DECREF(pyDict, "i", PyLong_FromUnsignedLong(i));
            PyDict_SetItemString_DECREF(pyDict, "va", PyLong_FromUnsignedLongLong(pe->vaFunction));
            PyDict_SetItemString_DECREF(pyDict, "fn", PyUnicode_FromFormat("%s", pe->szFunction));
            PyDict_SetItemString_DECREF(pyDict, "dll", PyUnicode_FromFormat("%s", pe->szModule));
            PyList_Append_DECREF(pyList, pyDict);
        }
    }
    LocalFree(pIATs);
    return pyList;
}

// (PBYTE, (DWORD)) -> STR
static PyObject*
VMMPYC_UtilFillHexAscii(PyObject *self, PyObject *args)
{
    PyObject *pyString;
    DWORD cb, cbInitialOffset = 0, csz = 0;
    PBYTE pb, pbPy;
    LPSTR sz = NULL;
    BOOL result;
    if(!PyArg_ParseTuple(args, "y#|k", &pbPy, &cb, &cbInitialOffset)) { return NULL; }
    if(cb == 0) {
        return PyUnicode_FromFormat("%s", "");
    }
    pb = LocalAlloc(0, cb);
    if(!pb) {
        return PyErr_NoMemory();
    }
    memcpy(pb, pbPy, cb);
    Py_BEGIN_ALLOW_THREADS;
    result =
        VMMDLL_UtilFillHexAscii(pb, cb, cbInitialOffset, NULL, &csz) &&
        csz &&
        (sz = (LPSTR)LocalAlloc(0, csz)) &&
        VMMDLL_UtilFillHexAscii(pb, cb, cbInitialOffset, sz, &csz);
    LocalFree(pb);
    Py_END_ALLOW_THREADS;
    if(!result || !sz) {
        LocalFree(sz);
        return PyErr_Format(PyExc_RuntimeError, "VMMPYC_UtilFillHexAscii: Failed.");
    }
    pyString = PyUnicode_FromFormat("%s", sz);
    LocalFree(sz);
    return pyString;
}

// (STR, DWORD, (ULONG64)) -> PBYTE
static PyObject*
VMMPYC_VfsRead(PyObject *self, PyObject *args)
{
    PyObject *pyBytes, *pyUnicodePath;
    NTSTATUS nt;
    DWORD cb, cbRead = 0;
    ULONG64 cbOffset = 0;
    PBYTE pb;
    LPWSTR wszPath = NULL;
    if(!PyArg_ParseTuple(args, "O!k|K", &PyUnicode_Type, &pyUnicodePath, &cb, &cbOffset)) { return NULL; }  // pyUnicodePath == borrowed reference - do not decrement
    if(cb > 0x01000000) { return PyErr_Format(PyExc_RuntimeError, "VMMPYC_VfsRead: Read larger than maximum supported (0x01000000) bytes requested."); }
    if(!(wszPath = PyUnicode_AsWideCharString(pyUnicodePath, NULL))) { return NULL; }                       // wszPath PyMem_Free() required 
    pb = LocalAlloc(0, cb);
    if(!pb) { return PyErr_NoMemory(); }
    Py_BEGIN_ALLOW_THREADS;
    nt = VMMDLL_VfsRead(wszPath, pb, cb, &cbRead, cbOffset);
    Py_END_ALLOW_THREADS;
    PyMem_Free(wszPath); wszPath = NULL;
    if(nt != VMMDLL_STATUS_SUCCESS) {
        LocalFree(pb);
        return PyErr_Format(PyExc_RuntimeError, "VMMPYC_VfsRead: Failed.");
    }
    pyBytes = PyBytes_FromStringAndSize(pb, cbRead);
    LocalFree(pb);
    return pyBytes;
}

// (STR, PBYTE, (ULONG64)) -> None
static PyObject*
VMMPYC_VfsWrite(PyObject *self, PyObject *args)
{
    PyObject *pyUnicodePath;
    BOOL result;
    DWORD cb, cbWritten;
    ULONG64 cbOffset;
    PBYTE pb, pbPy;
    LPWSTR wszPath = NULL;
    if(!PyArg_ParseTuple(args, "O!y#|K", &PyUnicode_Type, &pyUnicodePath, &pbPy, &cb, &cbOffset)) { return NULL; }  // pyUnicodePath == borrowed reference - do not decrement
    if(cb == 0) {
        return Py_BuildValue("s", NULL);    // zero-byte write is always successful.
    }
    if(!(wszPath = PyUnicode_AsWideCharString(pyUnicodePath, NULL))) { return NULL; }       // wszPath PyMem_Free() required 
    pb = LocalAlloc(0, cb);
    if(!pb) {
        return PyErr_NoMemory();
    }
    memcpy(pb, pbPy, cb);
    Py_BEGIN_ALLOW_THREADS;
    result = (VMMDLL_STATUS_SUCCESS == VMMDLL_VfsWrite(wszPath, pb, cb, &cbWritten, cbOffset));
    LocalFree(pb);
    Py_END_ALLOW_THREADS;
    PyMem_Free(wszPath); wszPath = NULL;
    if(!result) { return PyErr_Format(PyExc_RuntimeError, "VMMPYC_VfsWrite: Failed."); }
    return Py_BuildValue("s", NULL);    // None returned on success.
}

// (DWORD, STR, STR) -> ULONG64
static PyObject*
VMMPYC_ProcessGetProcAddress(PyObject *self, PyObject *args)
{
    PyObject *pyUnicodeModule;
    ULONG64 va;
    DWORD dwPID;
    LPSTR szProcName;
    LPWSTR wszModule = NULL;
    if(!PyArg_ParseTuple(args, "kO!s", &dwPID, &PyUnicode_Type, &pyUnicodeModule, &szProcName)) { return NULL; }
    if(!(wszModule = PyUnicode_AsWideCharString(pyUnicodeModule, NULL))) { return NULL; }       // wszPath PyMem_Free() required
    Py_BEGIN_ALLOW_THREADS;
    va = VMMDLL_ProcessGetProcAddress(dwPID, wszModule, szProcName);
    Py_END_ALLOW_THREADS;
    PyMem_Free(wszModule);
    return va ?
        PyLong_FromUnsignedLongLong(va) :
        PyErr_Format(PyExc_RuntimeError, "VMMPYC_ProcessGetProcAddress: Failed.");
}

// (DWORD, STR) -> ULONG64
static PyObject*
VMMPYC_ProcessGetModuleBase(PyObject *self, PyObject *args)
{
    PyObject *pyUnicodeModule;
    ULONG64 va;
    DWORD dwPID;
    LPWSTR wszModule = NULL;
    if(!PyArg_ParseTuple(args, "kO!", &dwPID, &PyUnicode_Type, &pyUnicodeModule)) { return NULL; }
    if(!(wszModule = PyUnicode_AsWideCharString(pyUnicodeModule, NULL))) { return NULL; }       // wszPath PyMem_Free() required
    Py_BEGIN_ALLOW_THREADS;
    va = VMMDLL_ProcessGetModuleBase(dwPID, wszModule);
    Py_END_ALLOW_THREADS;
    PyMem_Free(wszModule);
    return va ?
        PyLong_FromUnsignedLongLong(va) :
        PyErr_Format(PyExc_RuntimeError, "VMMPYC_ProcessGetModuleBase: Failed.");
}

// (DWORD, STR, STR) -> {...}
static PyObject*
VMMPYC_WinGetThunkInfoEAT(PyObject *self, PyObject *args)
{
    PyObject *pyDict, *pyUnicodeModule;
    BOOL result;
    DWORD dwPID;
    VMMDLL_WIN_THUNKINFO_EAT oThunkInfoEAT = { 0 };
    LPSTR szExportFunctionName;
    LPWSTR wszModule = NULL;
    if(!PyArg_ParseTuple(args, "kO!s", &dwPID, &PyUnicode_Type, &pyUnicodeModule, &szExportFunctionName)) { return NULL; }
    if(!(wszModule = PyUnicode_AsWideCharString(pyUnicodeModule, NULL))) { return NULL; }       // wszPath PyMem_Free() required
    Py_BEGIN_ALLOW_THREADS;
    result = VMMDLL_WinGetThunkInfoEAT(dwPID, wszModule, szExportFunctionName, &oThunkInfoEAT);
    Py_END_ALLOW_THREADS;
    PyMem_Free(wszModule);
    if(!result || !oThunkInfoEAT.fValid) {
        return PyErr_Format(PyExc_RuntimeError, "VMMPYC_WinGetThunkInfoEAT: Failed.");
    }
    pyDict = PyDict_New();
    if(pyDict) {
        PyDict_SetItemString_DECREF(pyDict, "vaFunction", PyLong_FromUnsignedLongLong(oThunkInfoEAT.vaFunction));
        PyDict_SetItemString_DECREF(pyDict, "valueThunk", PyLong_FromUnsignedLong(oThunkInfoEAT.valueThunk));
        PyDict_SetItemString_DECREF(pyDict, "vaNameFunction", PyLong_FromUnsignedLongLong(oThunkInfoEAT.vaNameFunction));
        PyDict_SetItemString_DECREF(pyDict, "vaThunk", PyLong_FromUnsignedLongLong(oThunkInfoEAT.vaThunk));
    }
    return pyDict;
}

// (DWORD, STR, STR, STR) -> {...}
static PyObject*
VMMPYC_WinGetThunkInfoIAT(PyObject *self, PyObject *args)
{
    PyObject *pyDict, *pyUnicodeModule;
    BOOL result;
    DWORD dwPID;
    VMMDLL_WIN_THUNKINFO_IAT oThunkInfoIAT = { 0 };
    LPSTR szImportModuleName, szImportFunctionName;
    LPWSTR wszModule = NULL;
    if(!PyArg_ParseTuple(args, "kO!ss", &dwPID, &PyUnicode_Type, &pyUnicodeModule, &szImportModuleName, &szImportFunctionName)) { return NULL; }
    if(!(wszModule = PyUnicode_AsWideCharString(pyUnicodeModule, NULL))) { return NULL; }       // wszPath PyMem_Free() required
    Py_BEGIN_ALLOW_THREADS;
    result = VMMDLL_WinGetThunkInfoIAT(dwPID, wszModule, szImportModuleName, szImportFunctionName, &oThunkInfoIAT);
    Py_END_ALLOW_THREADS;
    PyMem_Free(wszModule);
    if(!result || !oThunkInfoIAT.fValid) {
        return PyErr_Format(PyExc_RuntimeError, "VMMPYC_WinGetThunkInfoEAT: Failed.");
    }
    pyDict = PyDict_New();
    if(pyDict) {
        PyDict_SetItemString_DECREF(pyDict, "32", PyBool_FromLong(oThunkInfoIAT.f32 ? 1 : 0));
        PyDict_SetItemString_DECREF(pyDict, "vaFunction", PyLong_FromUnsignedLongLong(oThunkInfoIAT.vaFunction));
        PyDict_SetItemString_DECREF(pyDict, "vaNameFunction", PyLong_FromUnsignedLongLong(oThunkInfoIAT.vaNameFunction));
        PyDict_SetItemString_DECREF(pyDict, "vaNameModule", PyLong_FromUnsignedLongLong(oThunkInfoIAT.vaNameModule));
        PyDict_SetItemString_DECREF(pyDict, "vaThunk", PyLong_FromUnsignedLongLong(oThunkInfoIAT.vaThunk));
    }
    return pyDict;
}

// () -> [{...}]
static PyObject *
VMMPYC_WinReg_HiveList(PyObject *self, PyObject *args)
{
    PyObject *pyList, *pyDict;
    BOOL result;
    DWORD i, cHives;
    PVMMDLL_REGISTRY_HIVE_INFORMATION pe, pHives = NULL;
    if(!(pyList = PyList_New(0))) { return PyErr_NoMemory(); }
    Py_BEGIN_ALLOW_THREADS;
    VMMDLL_WinReg_HiveList(NULL, 0, &cHives);
    result =
        VMMDLL_WinReg_HiveList(NULL, 0, &cHives) &&
        cHives &&
        (pHives = LocalAlloc(0, cHives * sizeof(VMMDLL_REGISTRY_HIVE_INFORMATION))) &&
        VMMDLL_WinReg_HiveList(pHives, cHives, &cHives);
    Py_END_ALLOW_THREADS;
    if(!result) {
        Py_DECREF(pyList);
        LocalFree(pHives);
        return PyErr_Format(PyExc_RuntimeError, "VMMPYC_WinRegHive_List: Failed.");
    }
    for(i = 0; i < cHives; i++) {
        if((pyDict = PyDict_New())) {
            pe = pHives + i;
            PyDict_SetItemString_DECREF(pyDict, "i", PyLong_FromUnsignedLong(i));
            PyDict_SetItemString_DECREF(pyDict, "va_hive", PyLong_FromUnsignedLongLong(pe->vaCMHIVE));
            PyDict_SetItemString_DECREF(pyDict, "va_baseblock", PyLong_FromUnsignedLongLong(pe->vaHBASE_BLOCK));
            PyDict_SetItemString_DECREF(pyDict, "name", PyUnicode_FromFormat("%s", pe->szName));
            PyList_Append_DECREF(pyList, pyDict);
        }
    }
    LocalFree(pHives);
    return pyList;
}

// (ULONG64, DWORD, DWORD, (ULONG64)) -> PBYTE
static PyObject*
VMMPYC_WinReg_HiveRead(PyObject *self, PyObject *args)
{
    PyObject *pyBytes;
    BOOL result;
    DWORD ra, cb, cbRead = 0;
    ULONG64 vaHive, flags = 0;
    PBYTE pb;
    if(!PyArg_ParseTuple(args, "Kkk|K", &vaHive, &ra, &cb, &flags)) { return NULL; }
    if(cb > 0x01000000) { return PyErr_Format(PyExc_RuntimeError, "VMMPYC_WinRegHive_Read: Read larger than maximum supported (0x01000000) bytes requested."); }
    pb = LocalAlloc(0, cb);
    if(!pb) { return PyErr_NoMemory(); }
    Py_BEGIN_ALLOW_THREADS;
    result = VMMDLL_WinReg_HiveReadEx(vaHive, ra, pb, cb, &cbRead, flags);
    Py_END_ALLOW_THREADS;
    if(!result) {
        LocalFree(pb);
        return PyErr_Format(PyExc_RuntimeError, "VMMPYC_WinRegHive_Read: Failed.");
    }
    pyBytes = PyBytes_FromStringAndSize(pb, cbRead);
    LocalFree(pb);
    return pyBytes;
}

// (ULONG64, DWORD, PBYTE) -> None
static PyObject *
VMMPYC_WinReg_HiveWrite(PyObject *self, PyObject *args)
{
    BOOL result;
    DWORD ra;
    ULONG64 vaHive;
    PBYTE pb, pbPy;
    DWORD cb;
    if(!PyArg_ParseTuple(args, "Kky#", &vaHive, &ra, &pbPy, &cb)) { return NULL; }
    if(cb == 0) {
        return Py_BuildValue("s", NULL);    // zero-byte write is always successful.
    }
    pb = LocalAlloc(0, cb);
    if(!pb) {
        return PyErr_NoMemory();
    }
    memcpy(pb, pbPy, cb);
    Py_BEGIN_ALLOW_THREADS;
    result = VMMDLL_WinReg_HiveWrite(vaHive, ra, pb, (DWORD)cb);
    LocalFree(pb);
    Py_END_ALLOW_THREADS;
    if(!result) { return PyErr_Format(PyExc_RuntimeError, "VMMPYC_WinRegHive_Write: Failed."); }
    return Py_BuildValue("s", NULL);    // None returned on success.
}

// (WSTR) -> {...}
static PyObject*
VMMPYC_WinReg_EnumKey(PyObject *self, PyObject *args)
{
    PyObject *pyDict, *pyListKey, *pyDictKey, *pyListValue, *pyDictValue, *pyName;
    PyObject *pyUnicodePathKey;             // must not be DECREF'ed
    BOOL fResult;
    LPWSTR wszPathKey = NULL;               // PyMem_Free() required
    WCHAR wsz[MAX_PATH];
    CHAR szTime[MAX_PATH];
    DWORD i, cch, dwType, cbData = 0;
    FILETIME ftKeyLastWriteTime;
    if(!PyArg_ParseTuple(args, "O!", &PyUnicode_Type, &pyUnicodePathKey)) { return NULL; }
    if(!(wszPathKey = PyUnicode_AsWideCharString(pyUnicodePathKey, NULL))) {
        PyErr_Clear();
        PyMem_Free(wszPathKey);
        return PyErr_Format(PyExc_RuntimeError, "VMMPYC_WinReg_EnumKey: Failed parse key/value path.");
    }
    if((pyDict = PyDict_New())) {
        // key list
        if((pyListKey = PyList_New(0))) {
            i = 0;
            while(TRUE) {
                Py_BEGIN_ALLOW_THREADS;
                cch = _countof(wsz);
                fResult = VMMDLL_WinReg_EnumKeyExW(wszPathKey, i++, wsz, &cch, &ftKeyLastWriteTime);
                Py_END_ALLOW_THREADS;
                if(!fResult) { break; }
                if((pyDictKey = PyDict_New())) {
                    if((pyName = PyUnicode_FromWideChar(wsz, -1))) {
                        Util_FileTime2String(&ftKeyLastWriteTime, szTime);
                        PyDict_SetItemString_DECREF(pyDictKey, "name", pyName);
                        PyDict_SetItemString_DECREF(pyDictKey, "time", PyLong_FromUnsignedLongLong(*(PQWORD)&ftKeyLastWriteTime));
                        PyDict_SetItemString_DECREF(pyDictKey, "time-str", PyUnicode_FromFormat("%s", szTime));
                    }
                    PyList_Append_DECREF(pyListKey, pyDictKey);
                }
            }
            PyDict_SetItemString_DECREF(pyDict, "subkeys", pyListKey);
        }
        // value list
        if((pyListValue = PyList_New(0))) {
            i = 0;
            while(TRUE) {
                Py_BEGIN_ALLOW_THREADS;
                cch = _countof(wsz);
                fResult = VMMDLL_WinReg_EnumValueW(wszPathKey, i++, wsz, &cch, &dwType, NULL, &cbData);
                Py_END_ALLOW_THREADS;
                if(!fResult) { break; }
                if((pyDictValue = PyDict_New())) {
                    if((pyName = PyUnicode_FromWideChar(wsz, -1))) {
                        PyDict_SetItemString_DECREF(pyDictValue, "name", pyName);
                        PyDict_SetItemString_DECREF(pyDictValue, "type", PyLong_FromUnsignedLong(dwType));
                        PyDict_SetItemString_DECREF(pyDictValue, "size", PyLong_FromUnsignedLong(cbData));
                    }
                    PyList_Append_DECREF(pyListValue, pyDictValue);
                }
            }
            PyDict_SetItemString_DECREF(pyDict, "values", pyListValue);
        }
    }
    PyMem_Free(wszPathKey);
    return pyDict;
}

// (WSTR) -> {...}
static PyObject*
VMMPYC_WinReg_QueryValue(PyObject *self, PyObject *args)
{
    PyObject *pyDict;
    PyObject *pyUnicodePathKeyValue;            // must not be DECREF'ed
    BOOL result;
    LPWSTR wszPathKeyValue = NULL;              // PyMem_Free() required
    DWORD dwType;
    DWORD cbData = 0x01000000;                  // 1MB
    PBYTE pbData = NULL;
    if(!(pbData = LocalAlloc(0, cbData))) { goto fail; }
    if(!PyArg_ParseTuple(args, "O!", &PyUnicode_Type, &pyUnicodePathKeyValue)) { return NULL; }
    if(!(wszPathKeyValue = PyUnicode_AsWideCharString(pyUnicodePathKeyValue, NULL))) { goto fail; }
    Py_BEGIN_ALLOW_THREADS;
    result = VMMDLL_WinReg_QueryValueExW(wszPathKeyValue, &dwType, pbData, &cbData);
    Py_END_ALLOW_THREADS;
    if(!result) {
        LocalFree(pbData);
        PyMem_Free(wszPathKeyValue);
        return PyErr_Format(PyExc_RuntimeError, "VMMPYC_WinReg_QueryValue: Failed.");
    }
    pyDict = PyDict_New();
    if(pyDict) {
        PyDict_SetItemString_DECREF(pyDict, "type", PyLong_FromUnsignedLong(dwType));
        PyDict_SetItemString_DECREF(pyDict, "data", PyBytes_FromStringAndSize(pbData, cbData));
    }
    LocalFree(pbData);
    PyMem_Free(wszPathKeyValue);
    return pyDict;
fail:
    PyErr_Clear();
    LocalFree(pbData);
    PyMem_Free(wszPathKeyValue);
    return PyErr_Format(PyExc_RuntimeError, "VMMPYC_WinReg_QueryValue: Failed parse key/value path.");
}

// () -> {'tcpe': [{...}]}
static PyObject*
VMMPYC_WinNet_Get(PyObject *self, PyObject *args)
{
    PyObject *pyDict, *pyListTcpE, *pyDictTcpE;
    DWORD i, dwIpVersion;
    PVMMDLL_WIN_TCPIP pNet = NULL;
    PVMMDLL_WIN_TCPIP_ENTRY pE;
    CHAR szSrc[64], szDst[64], szTime[MAX_PATH];
    if(!(pyDict = PyDict_New())) { return PyErr_NoMemory(); }
    if(!(pyListTcpE = PyList_New(0))) { return PyErr_NoMemory(); }
    PyDict_SetItemString_DECREF(pyDict, "TcpE", pyListTcpE);
    Py_BEGIN_ALLOW_THREADS;
    pNet = VMMDLL_WinNet_Get();
    Py_END_ALLOW_THREADS;
    if(!pNet || (pNet->magic != VMMDLL_WIN_TCPIP_MAGIC) || (pNet->dwVersion != VMMDLL_WIN_TCPIP_VERSION)) {
        VMMDLL_MemFree(pNet);
        Py_DECREF(pyDict);
        return PyErr_Format(PyExc_RuntimeError, "VMMPYC_WinNet_Get: Failed.");
    }
    // add tcp endpoint entries to TcpE list
    for(i = 0; i < pNet->cTcpE; i++) {
        if((pyDictTcpE = PyDict_New())) {
            pE = pNet->pTcpE + i;
            dwIpVersion = (pE->AF.wAF == AF_INET) ? 4 : ((pE->AF.wAF == AF_INET6) ? 6 : 0);
            // format src/dst addr
            szSrc[0] = 0;
            szDst[0] = 0;
            if(pE->Src.fValid) {
                InetNtopA(pE->AF.wAF, pE->Src.pbA, szSrc, sizeof(szSrc));
            }
            if(pE->Dst.fValid) {
                InetNtopA(pE->AF.wAF, pE->Dst.pbA, szDst, sizeof(szDst));
            }
            // get time
            Util_FileTime2String((PFILETIME)&pE->qwTime, szTime);
            PyDict_SetItemString_DECREF(pyDictTcpE, "ver", PyLong_FromUnsignedLong(dwIpVersion));
            PyDict_SetItemString_DECREF(pyDictTcpE, "pid", PyLong_FromUnsignedLong(pE->dwPID));
            PyDict_SetItemString_DECREF(pyDictTcpE, "state", PyLong_FromUnsignedLong(pE->dwState));
            PyDict_SetItemString_DECREF(pyDictTcpE, "va", PyLong_FromUnsignedLongLong(pE->vaTcpE));
            PyDict_SetItemString_DECREF(pyDictTcpE, "time", PyLong_FromUnsignedLongLong(pE->qwTime));
            PyDict_SetItemString_DECREF(pyDictTcpE, "time-str", PyUnicode_FromFormat("%s", szTime));
            PyDict_SetItemString_DECREF(pyDictTcpE, "src-ip", PyUnicode_FromFormat("%s", szSrc));
            PyDict_SetItemString_DECREF(pyDictTcpE, "src-port", PyLong_FromUnsignedLong(pE->Src.wPort));
            PyDict_SetItemString_DECREF(pyDictTcpE, "dst-ip", PyUnicode_FromFormat("%s", szDst));
            PyDict_SetItemString_DECREF(pyDictTcpE, "dst-port", PyLong_FromUnsignedLong(pE->Dst.wPort));
            PyList_Append_DECREF(pyListTcpE, pyDictTcpE);
        }
    }
    VMMDLL_MemFree(pNet);
    return pyDict;
}




// (STR, STR) -> ULONG64
static PyObject *
VMMPYC_PdbSymbolAddress(PyObject *self, PyObject *args)
{
    BOOL result;
    ULONG64 vaSymbol;
    LPSTR szModule, szTypeName;
    if(!PyArg_ParseTuple(args, "ss", &szModule, &szTypeName)) { return NULL; }
    Py_BEGIN_ALLOW_THREADS;
    result = VMMDLL_PdbSymbolAddress(szModule, szTypeName, &vaSymbol);
    Py_END_ALLOW_THREADS;
    if(!result) { return PyErr_Format(PyExc_RuntimeError, "VMMPYC_PdbSymbolAddress: Failed."); }
    return PyLong_FromUnsignedLongLong(vaSymbol);
}

// (STR, STR) -> ULONG
static PyObject *
VMMPYC_PdbTypeSize(PyObject *self, PyObject *args)
{
    BOOL result;
    DWORD dwSize;
    LPSTR szModule, szTypeName;
    if(!PyArg_ParseTuple(args, "ss", &szModule, &szTypeName)) { return NULL; }
    Py_BEGIN_ALLOW_THREADS;
    result = VMMDLL_PdbTypeSize(szModule, szTypeName, &dwSize);
    Py_END_ALLOW_THREADS;
    if(!result) { return PyErr_Format(PyExc_RuntimeError, "VMMPYC_PdbTypeSize: Failed."); }
    return PyLong_FromUnsignedLong(dwSize);
}

// (STR, STR, WSTR) -> ULONG
static PyObject *
VMMPYC_PdbTypeChildOffset(PyObject *self, PyObject *args)
{
    PyObject *pyTypeChildName;
    BOOL result;
    DWORD dwChildOffset;
    LPSTR szModule, szTypeName;
    LPWSTR wszTypeChildName = NULL;
    if(!PyArg_ParseTuple(args, "ssO!", &szModule, &szTypeName, &PyUnicode_Type, &pyTypeChildName)) { return NULL; }  // pyTypeChildName == borrowed reference - do not decrement
    if(!(wszTypeChildName = PyUnicode_AsWideCharString(pyTypeChildName, NULL))) { return NULL; }       // wszTypeChildName PyMem_Free() required 
    Py_BEGIN_ALLOW_THREADS;
    result = VMMDLL_PdbTypeChildOffset(szModule, szTypeName, wszTypeChildName, &dwChildOffset);
    Py_END_ALLOW_THREADS;
    PyMem_Free(wszTypeChildName); wszTypeChildName = NULL;
    if(!result) { return PyErr_Format(PyExc_RuntimeError, "VMMPYC_PdbTypeChildOffset: Failed."); }
    return PyLong_FromUnsignedLong(dwChildOffset);
}



typedef struct tdVMMPYC_VFSLIST {
    struct tdVMMPYC_VFSLIST *FLink;
    WCHAR wszName[MAX_PATH];
    BOOL fIsDir;
    ULONG64 qwSize;
} VMMPYC_VFSLIST, *PVMMPYC_VFSLIST;


VOID VMMPYC_VfsList_AddInternal(_Inout_ HANDLE h, _In_opt_ LPSTR szName, _In_opt_ LPWSTR wszName, _In_ ULONG64 size, _In_ BOOL fIsDirectory)
{
    DWORD i = 0;
    PVMMPYC_VFSLIST pE;
    PVMMPYC_VFSLIST *ppE = (PVMMPYC_VFSLIST*)h;
    if((pE = LocalAlloc(0, sizeof(VMMPYC_VFSLIST)))) {
        while(i < MAX_PATH && ((szName && szName[i]) || (wszName && wszName[i]))) {
            pE->wszName[i] = szName ? szName[i] : wszName[i];
            i++;
        }
        pE->wszName[min(i, MAX_PATH - 1)] = 0;
        pE->fIsDir = fIsDirectory;
        pE->qwSize = size;
        pE->FLink = *ppE;
        *ppE = pE;
    }
}

VOID VMMPYC_VfsList_AddFile(_Inout_ HANDLE h, _In_opt_ LPSTR szName, _In_opt_ LPWSTR wszName, _In_ ULONG64 size, _In_opt_ PVMMDLL_VFS_FILELIST_EXINFO pExInfo)
{
    VMMPYC_VfsList_AddInternal(h, szName, wszName, size, FALSE);
}

VOID VMMPYC_VfsList_AddDirectory(_Inout_ HANDLE h, _In_opt_ LPSTR szName, _In_opt_ LPWSTR wszName, _In_opt_ PVMMDLL_VFS_FILELIST_EXINFO pExInfo)
{
    VMMPYC_VfsList_AddInternal(h, szName, wszName, 0, TRUE);
}


// (STR) -> {{...}}
static PyObject*
VMMPYC_VfsList(PyObject *self, PyObject *args)
{
    PyObject *pyDict, *PyDict_Attr;
    PyObject *pyKeyName, *pyUnicodePath;
    BOOL result;
    LPWSTR wszPath = NULL;
    VMMDLL_VFS_FILELIST hFileList;
    PVMMPYC_VFSLIST pE = NULL, pE_Next;
    if(!PyArg_ParseTuple(args, "O!", &PyUnicode_Type, &pyUnicodePath)) { return NULL; }     // pyUnicodePath == borrowed reference - do not decrement
    if(!(wszPath = PyUnicode_AsWideCharString(pyUnicodePath, NULL))) { return NULL; }       // wszPath PyMem_Free() required 
    if(!(pyDict = PyDict_New())) { return PyErr_NoMemory(); }
    Py_BEGIN_ALLOW_THREADS;
    hFileList.h = &pE;
    hFileList.pfnAddFile = VMMPYC_VfsList_AddFile;
    hFileList.pfnAddDirectory = VMMPYC_VfsList_AddDirectory;
    hFileList.dwVersion = VMMDLL_VFS_FILELIST_VERSION;
    result = VMMDLL_VfsList(wszPath, &hFileList);
    pE = *(PVMMPYC_VFSLIST*)hFileList.h;
    Py_END_ALLOW_THREADS;
    PyMem_Free(wszPath); wszPath = NULL;
    while(pE) {
        if((PyDict_Attr = PyDict_New())) {
            PyDict_SetItemString_DECREF(PyDict_Attr, "f_isdir", PyBool_FromLong(pE->fIsDir ? 1 : 0));
            PyDict_SetItemString_DECREF(PyDict_Attr, "size", PyLong_FromUnsignedLongLong(pE->qwSize));
            pyKeyName = PyUnicode_FromWideChar(pE->wszName, -1);
            PyDict_SetItem(pyDict, pyKeyName, PyDict_Attr);
            Py_DECREF(pyKeyName);
            Py_DECREF(PyDict_Attr);
        }
        pE_Next = pE->FLink;
        LocalFree(pE);
        pE = pE_Next;
    }
    if(!result) {
        Py_DECREF(pyDict);
        return PyErr_Format(PyExc_RuntimeError, "VMMPYC_VfsList: Failed.");
    }
    return pyDict;
}

//-----------------------------------------------------------------------------
// PY2C common functionality below:
//-----------------------------------------------------------------------------

static PyMethodDef VMMPYC_EmbMethods[] = {
    {"VMMPYC_Initialize", VMMPYC_Initialize, METH_VARARGS, "Initialize the VMM"},
    {"VMMPYC_Close", VMMPYC_Close, METH_VARARGS, "Try close the VMM"},
    {"VMMPYC_Refresh", VMMPYC_Refresh, METH_VARARGS, "Force refresh the VMM (process listings and caches)."},
    {"VMMPYC_ConfigGet", VMMPYC_ConfigGet, METH_VARARGS, "Get a device specific option value."},
    {"VMMPYC_ConfigSet", VMMPYC_ConfigSet, METH_VARARGS, "Set a device specific option value."},
    {"VMMPYC_MemReadScatter", VMMPYC_MemReadScatter, METH_VARARGS, "Read multiple 4kB page sized and aligned chunks of memory given as an address list."},
    {"VMMPYC_MemRead", VMMPYC_MemRead, METH_VARARGS, "Read memory."},
    {"VMMPYC_MemWrite", VMMPYC_MemWrite, METH_VARARGS, "Write memory."},
    {"VMMPYC_MemVirt2Phys", VMMPYC_MemVirt2Phys, METH_VARARGS, "Translate a virtual address into a physical address."},
    {"VMMPYC_PidGetFromName", VMMPYC_PidGetFromName, METH_VARARGS, "Locate a process by name and return the PID."},
    {"VMMPYC_PidList", VMMPYC_PidList, METH_VARARGS, "List all process PIDs."},
    {"VMMPYC_ProcessGetPteMap", VMMPYC_ProcessGetPteMap, METH_VARARGS, "Retrieve the PTE memory map for a given process."},
    {"VMMPYC_ProcessGetVadMap", VMMPYC_ProcessGetVadMap, METH_VARARGS, "Retrieve the VAD memory map for a given process."},
    {"VMMPYC_ProcessGetModuleMap", VMMPYC_ProcessGetModuleMap, METH_VARARGS, "Retrieve the module map for a given process."},
    {"VMMPYC_ProcessGetModuleFromName", VMMPYC_ProcessGetModuleFromName, METH_VARARGS, "Locate a module by name and return its information."},
    {"VMMPYC_ProcessGetHeapMap", VMMPYC_ProcessGetHeapMap, METH_VARARGS, "Retrieve the heap map for a given process."},
    {"VMMPYC_ProcessGetThreadMap", VMMPYC_ProcessGetThreadMap, METH_VARARGS, "Retrieve the thread map for a given process."},
    {"VMMPYC_ProcessGetHandleMap", VMMPYC_ProcessGetHandleMap, METH_VARARGS, "Retrieve the handle map for a given process."},
    {"VMMPYC_ProcessGetInformation", VMMPYC_ProcessGetInformation, METH_VARARGS, "Retrieve process information for a specific process."},
    {"VMMPYC_ProcessGetDirectories", VMMPYC_ProcessGetDirectories, METH_VARARGS, "Retrieve the data directories for a specific process and module."},
    {"VMMPYC_ProcessGetSections", VMMPYC_ProcessGetSections, METH_VARARGS, "Retrieve the sections for a specific process and module."},
    {"VMMPYC_ProcessGetEAT", VMMPYC_ProcessGetEAT, METH_VARARGS, "Retrieve the export address table (EAT) for a specific process and module."},
    {"VMMPYC_ProcessGetIAT", VMMPYC_ProcessGetIAT, METH_VARARGS, "Retrieve the import address table (IAT) for a specific process and module."},
    {"VMMPYC_ProcessGetProcAddress", VMMPYC_ProcessGetProcAddress, METH_VARARGS, "Retrieve the proc address of a given module!function."},
    {"VMMPYC_ProcessGetModuleBase", VMMPYC_ProcessGetModuleBase, METH_VARARGS, "Retrieve the module base address given a module."},
    {"VMMPYC_WinGetThunkInfoEAT", VMMPYC_WinGetThunkInfoEAT, METH_VARARGS, "Retrieve information about the export address table (EAT) thunk. (useful for patching)."},
    {"VMMPYC_WinGetThunkInfoIAT", VMMPYC_WinGetThunkInfoIAT, METH_VARARGS, "Retrieve information about the import address table (IAT) thunk. (useful for patching)."},
    {"VMMPYC_WinReg_HiveList", VMMPYC_WinReg_HiveList, METH_VARARGS, "List registry hives."},
    {"VMMPYC_WinReg_HiveRead", VMMPYC_WinReg_HiveRead, METH_VARARGS, "Read raw registry hive."},
    {"VMMPYC_WinReg_HiveWrite", VMMPYC_WinReg_HiveWrite, METH_VARARGS, "Write raw registry hive."},
    {"VMMPYC_WinReg_EnumKey", VMMPYC_WinReg_EnumKey, METH_VARARGS, "Enumerate registry sub-keys."},
    {"VMMPYC_WinReg_QueryValue", VMMPYC_WinReg_QueryValue, METH_VARARGS, "Query registry value."},
    {"VMMPYC_WinNet_Get", VMMPYC_WinNet_Get, METH_VARARGS, "Retrieve windows networking information."},
    {"VMMPYC_PdbSymbolAddress", VMMPYC_PdbSymbolAddress, METH_VARARGS, "Retrieve debugging information - symbol address."},
    {"VMMPYC_PdbTypeSize", VMMPYC_PdbTypeSize, METH_VARARGS, "Retrieve debugging information - type size."},
    {"VMMPYC_PdbTypeChildOffset", VMMPYC_PdbTypeChildOffset, METH_VARARGS, "Retrieve debugging information - child offset."},
    {"VMMPYC_VfsRead", VMMPYC_VfsRead, METH_VARARGS, "Read from a file in the virtual file system."},
    {"VMMPYC_VfsWrite", VMMPYC_VfsWrite, METH_VARARGS, "Write to a file in the virtual file system."},
    {"VMMPYC_VfsList", VMMPYC_VfsList, METH_VARARGS, "List files and folder for a specific directory in the Virutal File System."},
    {"VMMPYC_UtilFillHexAscii", VMMPYC_UtilFillHexAscii, METH_VARARGS, "Convert a bytes object into a human readable 'memory dump' style type of string."},
    {NULL, NULL, 0, NULL}
};

static PyModuleDef VMMPYC_EmbModule = {
    PyModuleDef_HEAD_INIT, "vmmpyc", NULL, -1, VMMPYC_EmbMethods,
    NULL, NULL, NULL, NULL
};

__declspec(dllexport)
PyObject* PyInit_vmmpyc(void)
{
    return PyModule_Create(&VMMPYC_EmbModule);
}
