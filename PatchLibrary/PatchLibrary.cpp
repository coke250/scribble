﻿#include "plInternal.h"


namespace {

struct Symbol
{
    const char *name;
    void *addr;
    char code[8];

    bool operator<(const Symbol &other) const  { return strcmp(name, other.name)<0; }
    bool operator>(const Symbol &other) const  { return strcmp(name, other.name)>0; }
    bool operator==(const Symbol &other) const { return strcmp(name, other.name)==0; }
};

class DLL
{
public:
    typedef std::vector<Symbol> SymbolCont;
    typedef std::vector<void*> TrampolineCont;

    DLL();
    ~DLL();
    bool loadFile(const char *path);
    bool loadMemory(HMODULE mod);
    bool unload();
    void patch(DLL *patch);
    void unpatch();
    HMODULE getHandle() const { return m_module; }
    Symbol* findSymbol(const char *name)
    {
        Symbol tmp = {name, nullptr};
        auto iter = std::lower_bound(m_symbols.begin(), m_symbols.end(), tmp);
        if(iter!=m_symbols.end() && *iter==tmp) {
            return &(*iter);
        }
        return nullptr;
    }

private:
    SymbolCont m_symbols;
    TrampolineCont m_trampolines;
    std::string m_base_path;
    std::string m_dll_path;
    std::string m_pdb_path;
    HMODULE m_module;
    bool m_unload_required;
};

class PatchLibraryImpl
{
public:
    typedef std::map<std::string, DLL*> DLLCont;

    PatchLibraryImpl();
    ~PatchLibraryImpl();
    HMODULE loadAndPatch(const char *path);
    BOOL unpatchAndUnload(HMODULE mod);

private:
    DLLCont m_host_dlls;
    DLLCont m_patch_dlls;
};







DLL::DLL()
    : m_module(nullptr), m_unload_required(false)
{
}

DLL::~DLL()
{
    unload();
}

bool DLL::loadFile( const char *path )
{
    // ロード中の dll と関連する pdb はロックがかかってしまい、以降のビルドが失敗するようになるため、その対策を行う。
    // .dll と .pdb を一時ファイルにコピーしてそれをロードする。
    // コピーの際、
    // ・.dll に含まれる .pdb へのパスをコピー版へのパスに書き換える
    // ・.dll と .pdb 両方に含まれる pdb の GUID を更新する
    //   (VisualC++2012 の場合、これを怠ると最初にロードした dll の pdb が以降の更新された dll でも使われ続けてしまう)

    std::string pdb_base;
    GUID uuid;
    {
        // dll をメモリに map
        void *data = nullptr;
        size_t datasize = 0;
        if(!dpMapFile(path, data, datasize, malloc)) {
            printf("file not found %s\n", path);
            return false;
        }

        // 一時ファイル名を算出
        char rev[8] = {0};
        for(int i=0; i<0xfff; ++i) {
            _snprintf(rev, _countof(rev), "%x", i);
            m_dll_path = path;
            m_base_path.clear();
            std::string ext;
            dpSeparateFileExt(path, &m_base_path, &ext);
            m_base_path+=rev;
            m_base_path+=".";
            m_base_path+=ext;
            if(!dpFileExists(m_base_path.c_str())) { break; }
        }

        // pdb へのパスと GUID を更新
        if(CV_INFO_PDB70 *cv=dpGetPDBInfoFromModule(data, true)) {
            char *pdb = (char*)cv->PdbFileName;
            pdb_base = pdb;
            strncpy(pdb+pdb_base.size()-3, rev, 3);
            m_pdb_path = pdb;
            cv->Signature.Data1 += ::clock();
            uuid = cv->Signature;
        }
        // dll を一時ファイルにコピー
        dpWriteFile(m_base_path.c_str(), data, datasize);
        free(data);
    }

    {
        // pdb を GUID を更新してコピー
        void *data = nullptr;
        size_t datasize = 0;
        if(dpMapFile(pdb_base.c_str(), data, datasize, malloc)) {
            if(PDBStream70 *sig = dpGetPDBSignature(data)) {
                sig->sig70 = uuid;
            }
            dpWriteFile(m_pdb_path.c_str(), data, datasize);
            free(data);
        }
    }

    HMODULE module = ::LoadLibraryA(m_base_path.c_str());
    if(loadMemory(module)) {
        m_unload_required = true;
        return true;
    }
    else {
        printf("LoadLibraryA() failed. %s\n", path);
    }
    return false;
}

bool DLL::loadMemory(HMODULE data)
{
    m_module = (HMODULE)data;
    dpEnumerateDLLExports(m_module, [&](const char *name, void *addr){
        Symbol sym = {name, addr, {0}};
        memcpy(sym.code, addr, 8);
        m_symbols.push_back(sym);
    });
    std::sort(m_symbols.begin(), m_symbols.end());
    return true;
}

bool DLL::unload()
{
    bool ret = false;
    if(m_module) {
        if(m_unload_required) {
            ::FreeLibrary(m_module);
        }
        m_module = nullptr;
        m_unload_required = false;
        ret = true;
    }
    if(!m_dll_path.empty()) {
        dpDeleteFile(m_dll_path.c_str());
        m_dll_path.clear();
    }
    if(!m_pdb_path.empty()) {
        dpDeleteFile(m_pdb_path.c_str());
        m_pdb_path.clear();
    }
    m_base_path.clear();
    return ret;
}

void DLL::patch(DLL *patch)
{
    dpEach(m_symbols, [&](const Symbol &sym){
        if(Symbol *s = patch->findSymbol(sym.name)) {
            BYTE *from = (BYTE*)sym.addr;
            BYTE *to = (BYTE*)s->addr;
            HANDLE proc = ::GetCurrentProcess();
            DWORD old;
            ::VirtualProtect(sym.addr, 8, PAGE_EXECUTE_READWRITE, &old);

            // 距離が 32bit に収まらない場合、長距離 jmp で飛ぶコードを挟む。
            // (長距離 jmp は 14byte 必要なので直接書き込もうとすると容量が足りない可能性が出てくる)
            DWORD_PTR dwDistance = to < from ? from-to : to-from;
            if(dwDistance > 0x7fff0000) {
                //BYTE *trampoline = (BYTE*)m_talloc.allocate(from);
                //dpAddJumpInstruction(trampoline, to);
                //dpAddJumpInstruction(from, trampoline);
                //::FlushInstructionCache(proc, trampoline, 32);
                //::FlushInstructionCache(proc, from, 32);
                //pi.trampoline = trampoline;
                assert(0);
            }
            else {
                dpAddJumpInstruction(from, to);
                ::FlushInstructionCache(proc, from, 8);
            }

            ::VirtualProtect(sym.addr, 8, old, &old);
        }
    });
}

void DLL::unpatch()
{
    dpEach(m_symbols, [&](const Symbol &sym){
        DWORD old;
        ::VirtualProtect(sym.addr, 8, PAGE_EXECUTE_READWRITE, &old);
        memcpy(sym.addr, sym.code, 5);
        ::VirtualProtect(sym.addr, 8, old, &old);
    });
}



PatchLibraryImpl::PatchLibraryImpl()
{
}

PatchLibraryImpl::~PatchLibraryImpl()
{
    dpEach(m_patch_dlls, [&](DLLCont::value_type &p){ delete p.second; });
    m_patch_dlls.clear();
}

HMODULE PatchLibraryImpl::loadAndPatch( const char *path )
{
    std::string dir, file;
    dpSeparateDirFile(path, &dir, &file);
    dpEach(file, [](char &c){ c=::tolower(c); });

    DLL *&host_dll = m_host_dlls[file];
    if(!host_dll) {
        HMODULE mod = ::GetModuleHandleA(file.c_str());
        if(mod==nullptr) {
            return nullptr;
        }
        host_dll = new DLL();
        host_dll->loadMemory(mod);
    }

    DLL *&patch_dll = m_patch_dlls[file];
    if(patch_dll) {
        delete patch_dll;
        patch_dll = nullptr;
    }
    patch_dll = new DLL();
    patch_dll->loadFile(path);
    host_dll->patch(patch_dll);
    return patch_dll->getHandle();
}

BOOL PatchLibraryImpl::unpatchAndUnload( HMODULE mod )
{
    auto i = dpFind(m_patch_dlls, [&](DLLCont::value_type &p){ return p.second->getHandle()==mod; });
    if(i!=m_patch_dlls.end()) {
        delete i->second;
        m_patch_dlls.erase(i);
        return TRUE;
    }
    return FALSE;
}


PatchLibraryImpl g_impl;

} // namespace



HMODULE WINAPI PatchLibraryA(const char *path)
{
    return g_impl.loadAndPatch(path);
}

HMODULE WINAPI PatchLibraryW(const wchar_t *wpath)
{
    char path[MAX_PATH];

    size_t wlen = wcstombs(nullptr, wpath, 0);
    if(wlen==size_t(-1)) { return nullptr; }
    wcstombs(path, wpath, wlen);
    path[wlen] = '\0';

    return PatchLibraryA(path);
}

BOOL WINAPI UnpatchLibrary(HMODULE mod)
{
    return g_impl.unpatchAndUnload(mod);
}
