// Shader bookkeeping: each created shader gets a hash of its bytecode, its constant table
// (CTAB comment block: names -> registers) and a one-time dump in hovr\shaders\ with the
// D3DX disassembly. The compile hooks dump HLSL sources and link them to the bytecode.
#include "common.h"

#include <cstdio>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

struct ID3DXBuffer : IUnknown {
    virtual LPVOID STDMETHODCALLTYPE GetBufferPointer() = 0;
    virtual DWORD STDMETHODCALLTYPE GetBufferSize() = 0;
};
struct D3DXMACRO_ {
    LPCSTR Name;
    LPCSTR Definition;
};

namespace hv {

static std::mutex g_mx;
static std::unordered_map<void*, ShaderInfo> g_shaders;
static std::unordered_set<uint64_t> g_dumped;
static std::unordered_map<uint64_t, uint64_t> g_code_to_source;

static std::wstring shader_dir() {
    static std::wstring d = [] {
        std::wstring p = path_in(L"shaders");
        CreateDirectoryW(p.c_str(), nullptr);
        return p;
    }();
    return d;
}

static size_t bytecode_size(const DWORD* code) {
    const DWORD* p = code;
    int major = int((p[0] >> 8) & 0xFF);
    ++p;
    for (int guard = 0; guard < 1 << 20; ++guard) {
        DWORD t = *p;
        if (t == 0x0000FFFF) return size_t(p + 1 - code) * 4;
        if ((t & 0xFFFF) == 0xFFFE) {
            p += 1 + ((t >> 16) & 0x7FFF);
            continue;
        }
        if (major >= 2) p += 1 + ((t >> 24) & 0xF);
        else ++p;
    }
    return 0;
}

static std::vector<ShaderConst> parse_ctab(const DWORD* code, size_t size) {
    std::vector<ShaderConst> out;
    const DWORD* end = code + size / 4;
    for (const DWORD* p = code + 1; p < end;) {
        DWORD t = *p;
        if ((t & 0xFFFF) != 0xFFFE) break;  // comments come first
        DWORD len = (t >> 16) & 0x7FFF;
        if (len >= 2 && p[1] == MAKEFOURCC('C', 'T', 'A', 'B')) {
            auto base = reinterpret_cast<const uint8_t*>(p + 2);
            size_t blen = (len - 1) * 4;
            auto u32 = [&](size_t off) { return off + 4 <= blen ? *reinterpret_cast<const DWORD*>(base + off) : 0u; };
            auto u16 = [&](size_t off) { return off + 2 <= blen ? *reinterpret_cast<const WORD*>(base + off) : WORD(0); };
            DWORD count = u32(12), info = u32(16);
            for (DWORD i = 0; i < count && info + i * 20 + 20 <= blen; ++i) {
                size_t ci = info + i * 20;
                DWORD name = u32(ci);
                ShaderConst c;
                if (name < blen) c.name.assign(reinterpret_cast<const char*>(base + name), strnlen(reinterpret_cast<const char*>(base + name), blen - name));
                c.set = u16(ci + 4);
                c.index = u16(ci + 6);
                c.count = u16(ci + 8);
                out.push_back(c);
            }
            break;
        }
        p += 1 + len;
    }
    return out;
}

using D3DXDisassembleShader_t = HRESULT(WINAPI*)(const DWORD*, BOOL, LPCSTR, ID3DXBuffer**);

static void dump(const ShaderInfo& info, const DWORD* code, uint64_t source) {
    wchar_t name[64];
    swprintf_s(name, L"\\%s_%016llx.txt", info.vertex ? L"vs" : L"ps", info.hash);
    FILE* f = _wfopen((shader_dir() + name).c_str(), L"w");
    if (!f) return;
    if (source) fprintf(f, "// source: src_%016llx.hlsl\n", source);
    static const char* sets[] = {"bool", "int", "float", "sampler"};
    for (auto& c : info.consts) fprintf(f, "// %-28s %s%d..%d\n", c.name.c_str(), sets[c.set & 3], c.index, c.index + c.count - 1);
    static auto dis = [] {
        HMODULE m = GetModuleHandleW(L"d3dx9_43.dll");
        return m ? reinterpret_cast<D3DXDisassembleShader_t>(GetProcAddress(m, "D3DXDisassembleShader")) : nullptr;
    }();
    ID3DXBuffer* text = nullptr;
    if (dis && SUCCEEDED(dis(code, FALSE, nullptr, &text)) && text) {
        fwrite(text->GetBufferPointer(), 1, strnlen(static_cast<const char*>(text->GetBufferPointer()), text->GetBufferSize()), f);
        text->Release();
    }
    fclose(f);
}

void shader_created(void* shader, const DWORD* code, bool vertex) {
    size_t size = bytecode_size(code);
    ShaderInfo info;
    info.vertex = vertex;
    info.hash = fnv64(code, size);
    info.consts = parse_ctab(code, size);
    uint64_t source = 0;
    bool first;
    {
        std::lock_guard lock(g_mx);
        auto it = g_code_to_source.find(info.hash);
        if (it != g_code_to_source.end()) source = it->second;
        first = g_dumped.insert(info.hash).second;
        g_shaders[shader] = info;
    }
    if (first) dump(info, code, source);
}

const ShaderInfo* shader_info(void* shader) {
    std::lock_guard lock(g_mx);
    auto it = g_shaders.find(shader);
    return it == g_shaders.end() ? nullptr : &it->second;
}

void note_compiled(const void* code, size_t, uint64_t source_hash) {
    size_t size = bytecode_size(static_cast<const DWORD*>(code));
    std::lock_guard lock(g_mx);
    g_code_to_source[fnv64(code, size)] = source_hash;
}

// --- compile hooks ---

using D3DXCompileShader_t = HRESULT(WINAPI*)(LPCSTR, UINT, const D3DXMACRO_*, void*, LPCSTR, LPCSTR, DWORD,
                                             ID3DXBuffer**, ID3DXBuffer**, void**);
using D3DXAssembleShader_t = HRESULT(WINAPI*)(LPCSTR, UINT, const D3DXMACRO_*, void*, DWORD, ID3DXBuffer**, ID3DXBuffer**);
static D3DXCompileShader_t o_compile;
static D3DXAssembleShader_t o_assemble;

static uint64_t dump_source(const char* kind, LPCSTR src, UINT len, const D3DXMACRO_* defines, LPCSTR entry, LPCSTR profile) {
    std::string header = std::string("// ") + kind + " entry=" + (entry ? entry : "-") + " profile=" + (profile ? profile : "-") + "\n";
    for (auto d = defines; d && d->Name; ++d) header += std::string("// #define ") + d->Name + " " + (d->Definition ? d->Definition : "") + "\n";
    uint64_t h = fnv64(src, len) ^ fnv64(header.data(), header.size());
    {
        std::lock_guard lock(g_mx);
        if (!g_dumped.insert(h).second) return h;
    }
    wchar_t name[64];
    swprintf_s(name, L"\\src_%016llx.hlsl", h);
    if (FILE* f = _wfopen((shader_dir() + name).c_str(), L"wb")) {
        fwrite(header.data(), 1, header.size(), f);
        fwrite(src, 1, len, f);
        fclose(f);
    }
    return h;
}

static HRESULT WINAPI hk_compile(LPCSTR src, UINT len, const D3DXMACRO_* defines, void* include, LPCSTR entry,
                                 LPCSTR profile, DWORD flags, ID3DXBuffer** shader, ID3DXBuffer** errors, void** table) {
    uint64_t h = dump_source("compile", src, len, defines, entry, profile);
    HRESULT hr = o_compile(src, len, defines, include, entry, profile, flags, shader, errors, table);
    if (SUCCEEDED(hr) && shader && *shader) note_compiled((*shader)->GetBufferPointer(), (*shader)->GetBufferSize(), h);
    return hr;
}

static HRESULT WINAPI hk_assemble(LPCSTR src, UINT len, const D3DXMACRO_* defines, void* include, DWORD flags,
                                  ID3DXBuffer** shader, ID3DXBuffer** errors) {
    uint64_t h = dump_source("assemble", src, len, defines, nullptr, nullptr);
    HRESULT hr = o_assemble(src, len, defines, include, flags, shader, errors);
    if (SUCCEEDED(hr) && shader && *shader) note_compiled((*shader)->GetBufferPointer(), (*shader)->GetBufferSize(), h);
    return hr;
}

void install_compile_hooks() {
    HMODULE exe = GetModuleHandleW(nullptr);
    patch_import(exe, "d3dx9_43.dll", "D3DXCompileShader", reinterpret_cast<void*>(hk_compile), reinterpret_cast<void**>(&o_compile));
    patch_import(exe, "d3dx9_43.dll", "D3DXAssembleShader", reinterpret_cast<void*>(hk_assemble), reinterpret_cast<void**>(&o_assemble));
}

}  // namespace hv
