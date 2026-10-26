// =====================================================================
// 最小化 OLE2 / CFB 只读读取器（Win32 原生版）
//
// 与 C# 版 RevitVersion.Core/CompoundFileReader.cs 逻辑一致，独立实现。
// 只做三件事：读 FAT / 目录项 / 指定名字的流。
// 不解析属性集、不写回、不校验完整性。
//
// 关键坑（已复现过，勿改回）：
//   扇区起始偏移恒为 sectorSize * (sectorId + 1)。
//   v3(sectorShift=9) 首扇区在 512；v4(sectorShift=12) 头部补齐到 4096，首扇区在 4096。
//   硬编码 512 + id*size 会让所有 4096 扇区文件解析失败。
// =====================================================================
#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <objidl.h>
#include <vector>
#include <string>
#include <stdint.h>

namespace cfbconst
{
    const uint32_t kMaxRegSect = 0xFFFFFFFAu;
    const uint32_t kEndOfChain = 0xFFFFFFFEu;
    const uint32_t kFreeSect   = 0xFFFFFFFFu;
    const int      kDirEntrySz = 128;
}

// 数据源抽象：既支持 IStream（shell 常见路径），也支持 HANDLE
class CfbSource
{
public:
    virtual ~CfbSource() {}
    virtual bool ReadAt(uint64_t offset, void* buf, uint32_t count) = 0;
};

class StreamSource : public CfbSource
{
public:
    explicit StreamSource(IStream* s) : _s(s) { if (_s) _s->AddRef(); }
    ~StreamSource() { if (_s) _s->Release(); }

    bool ReadAt(uint64_t offset, void* buf, uint32_t count) override
    {
        if (!_s) return false;
        LARGE_INTEGER li; li.QuadPart = (LONGLONG)offset;
        if (FAILED(_s->Seek(li, STREAM_SEEK_SET, nullptr))) return false;

        uint8_t* p = (uint8_t*)buf;
        uint32_t done = 0;
        while (done < count)
        {
            ULONG got = 0;
            HRESULT hr = _s->Read(p + done, count - done, &got);
            if (FAILED(hr) || got == 0) break;
            done += got;
        }
        return done == count;
    }

private:
    IStream* _s;
};

class HandleSource : public CfbSource
{
public:
    explicit HandleSource(HANDLE h) : _h(h) {}

    bool ReadAt(uint64_t offset, void* buf, uint32_t count) override
    {
        if (_h == INVALID_HANDLE_VALUE) return false;

        uint8_t* p = (uint8_t*)buf;
        uint32_t done = 0;
        while (done < count)
        {
            DWORD got = 0;
            OVERLAPPED cur;
            ZeroMemory(&cur, sizeof(cur));
            uint64_t off = offset + done;
            cur.Offset     = (DWORD)(off & 0xFFFFFFFF);
            cur.OffsetHigh = (DWORD)(off >> 32);

            if (!ReadFile(_h, p + done, count - done, &got, &cur))
            {
                if (GetLastError() == ERROR_HANDLE_EOF) break;
                return false;
            }
            if (got == 0) break;
            done += got;
        }
        return done == count;
    }

private:
    HANDLE _h;
};

struct CfbDirEntry
{
    std::wstring name;
    uint8_t      type = 0;      // 0=empty 1=storage 2=stream 5=root
    uint32_t     start = 0;
    uint64_t     size = 0;
};

class CfbReader
{
public:
    explicit CfbReader(IStream* s) { _src = new StreamSource(s); }
    explicit CfbReader(HANDLE h)    { _src = new HandleSource(h); }

    ~CfbReader() { delete _src; }

    // 打开并解析头部 / FAT / 目录 / MiniFAT / MiniStream
    bool Open()
    {
        uint8_t hdr[512] = { 0 };
        if (!_src->ReadAt(0, hdr, sizeof(hdr))) return false;

        // 签名 D0 CF 11 E0 A1 B1 1A E1
        if (!(hdr[0] == 0xD0 && hdr[1] == 0xCF && hdr[2] == 0x11 && hdr[3] == 0xE0 &&
              hdr[4] == 0xA1 && hdr[5] == 0xB1 && hdr[6] == 0x1A && hdr[7] == 0xE1))
            return false;

        uint16_t sectorShift     = *(uint16_t*)(hdr + 30);
        uint16_t miniSectorShift = *(uint16_t*)(hdr + 32);
        if (sectorShift != 9 && sectorShift != 12) return false;

        _sectorSize     = 1u << sectorShift;
        _miniSectorSize = 1u << miniSectorShift;
        _miniCutoff     = *(uint32_t*)(hdr + 56);

        int32_t  fatSectorCount     = *(int32_t*)(hdr + 44);
        uint32_t firstDirSector     = *(uint32_t*)(hdr + 48);
        uint32_t firstMiniFatSector = *(uint32_t*)(hdr + 60);
        uint32_t firstDifatSector   = *(uint32_t*)(hdr + 68);
        int32_t  difatSectorCount   = *(int32_t*)(hdr + 72);

        if (fatSectorCount < 0 || fatSectorCount > (1 << 22)) return false;

        // ---- DIFAT：头部 109 项 + 链式 DIFAT 扇区 ----
        std::vector<uint32_t> fatSectors;
        fatSectors.reserve((size_t)fatSectorCount);

        for (int i = 0; i < 109 && (int)fatSectors.size() < fatSectorCount; i++)
        {
            uint32_t sid = *(uint32_t*)(hdr + 76 + i * 4);
            if (sid > cfbconst::kMaxRegSect) break;
            fatSectors.push_back(sid);
        }

        uint32_t difat = firstDifatSector;
        int guard = 0;
        std::vector<uint8_t> buf(_sectorSize);
        while (difat <= cfbconst::kMaxRegSect &&
               difatSectorCount > 0 &&
               guard++ < difatSectorCount + 8 &&
               (int)fatSectors.size() < fatSectorCount)
        {
            if (!ReadSector(difat, buf)) break;
            uint32_t per = _sectorSize / 4 - 1;
            for (uint32_t i = 0; i < per && (int)fatSectors.size() < fatSectorCount; i++)
            {
                uint32_t sid = *(uint32_t*)(buf.data() + i * 4);
                if (sid > cfbconst::kMaxRegSect) break;
                fatSectors.push_back(sid);
            }
            difat = *(uint32_t*)(buf.data() + _sectorSize - 4);
        }

        // ---- FAT ----
        uint32_t perSector = _sectorSize / 4;
        _fat.clear();
        _fat.reserve(fatSectors.size() * perSector);
        for (uint32_t fsid : fatSectors)
        {
            if (!ReadSector(fsid, buf)) break;
            for (uint32_t i = 0; i < perSector; i++)
                _fat.push_back(*(uint32_t*)(buf.data() + i * 4));
        }
        if (_fat.empty()) return false;

        // ---- 目录 ----
        std::vector<uint8_t> dirData;
        if (!ReadChain(firstDirSector, 0, dirData)) return false;

        for (size_t off = 0; off + cfbconst::kDirEntrySz <= dirData.size(); off += cfbconst::kDirEntrySz)
        {
            uint16_t nameLen = *(uint16_t*)(dirData.data() + off + 64);
            uint8_t  type    = dirData[off + 66];
            if (type == 0) continue;
            if (nameLen < 2 || nameLen > 64) continue;

            CfbDirEntry e;
            e.name.assign((const wchar_t*)(dirData.data() + off), (size_t)(nameLen - 2) / 2);
            e.type  = type;
            e.start = *(uint32_t*)(dirData.data() + off + 116);
            // Size 字段 8 字节；v3 只用低 4 字节，这里统一读 8 字节低位
            e.size  = *(uint32_t*)(dirData.data() + off + 120);
            _entries.push_back(e);
        }

        // ---- MiniFAT ----
        _miniFat.clear();
        if (firstMiniFatSector <= cfbconst::kMaxRegSect)
        {
            std::vector<uint8_t> mf;
            if (ReadChain(firstMiniFatSector, 0, mf))
            {
                _miniFat.resize(mf.size() / 4);
                for (size_t i = 0; i < _miniFat.size(); i++)
                    _miniFat[i] = *(uint32_t*)(mf.data() + i * 4);
            }
        }

        // ---- Root entry / mini stream ----
        if (!_entries.empty() && _entries[0].type == 5 && _entries[0].size > 0)
            ReadChain(_entries[0].start, _entries[0].size, _miniStream);

        return !_entries.empty();
    }

    // 读取指定名字的流
    bool ReadStream(const wchar_t* name, std::vector<uint8_t>& out)
    {
        const CfbDirEntry* e = Find(name);
        if (!e) return false;

        if (e->size < _miniCutoff)
            return ReadMiniChain(e->start, e->size, out);

        return ReadChain(e->start, e->size, out);
    }

    // 读原始文件头若干字节（兜底扫描用）
    bool ReadHead(uint32_t count, std::vector<uint8_t>& out)
    {
        out.assign(count, 0);
        if (!_src->ReadAt(0, out.data(), count))
        {
            // 文件可能小于 count，逐个回退
            for (uint32_t n = count / 2; n >= 512; n /= 2)
            {
                out.assign(n, 0);
                if (_src->ReadAt(0, out.data(), n)) return true;
            }
            out.clear();
            return false;
        }
        return true;
    }

private:
    const CfbDirEntry* Find(const wchar_t* name)
    {
        for (const auto& e : _entries)
        {
            if (e.type == 2 && _wcsicmp(e.name.c_str(), name) == 0)
                return &e;
        }
        return nullptr;
    }

    bool ReadSector(uint32_t sectorId, std::vector<uint8_t>& out)
    {
        if (out.size() < _sectorSize) out.resize(_sectorSize);
        uint64_t offset = (uint64_t)_sectorSize * ((uint64_t)sectorId + 1);
        return _src->ReadAt(offset, out.data(), _sectorSize);
    }

    bool ReadMiniChain(uint32_t startSector, uint64_t size, std::vector<uint8_t>& out)
    {
        out.clear();
        if (size == 0) return true;
        if (_miniStream.empty()) return false;

        out.reserve((size_t)size);
        uint32_t sector = startSector;
        size_t guard = 0;
        size_t maxChain = _miniFat.size() + 1;

        while (sector <= cfbconst::kMaxRegSect && out.size() < size && guard++ < maxChain)
        {
            uint64_t offset = (uint64_t)sector * _miniSectorSize;
            if (offset + _miniSectorSize > _miniStream.size()) break;

            size_t toCopy = (size_t)((size - out.size()) < _miniSectorSize
                                     ? (size - out.size()) : _miniSectorSize);
            out.insert(out.end(),
                       _miniStream.begin() + (size_t)offset,
                       _miniStream.begin() + (size_t)offset + toCopy);

            if (sector >= _miniFat.size()) break;
            sector = _miniFat[sector];
        }
        return !out.empty();
    }

    // size == 0 表示「读到链结束」
    bool ReadChain(uint32_t startSector, uint64_t size, std::vector<uint8_t>& out)
    {
        out.clear();
        bool bounded = size > 0;
        if (bounded) out.reserve((size_t)size);

        uint32_t sector = startSector;
        size_t guard = 0;
        size_t maxSectors = _fat.size() + 1;
        std::vector<uint8_t> buf(_sectorSize);

        while (sector <= cfbconst::kMaxRegSect && guard++ < maxSectors)
        {
            if (bounded && out.size() >= size) break;
            if (!ReadSector(sector, buf)) break;

            if (bounded)
            {
                size_t remain = (size_t)(size - out.size());
                size_t n = remain < _sectorSize ? remain : _sectorSize;
                out.insert(out.end(), buf.begin(), buf.begin() + n);
            }
            else
            {
                out.insert(out.end(), buf.begin(), buf.end());
            }

            if (sector >= _fat.size()) break;
            sector = _fat[sector];
        }

        if (bounded && out.size() > size) out.resize((size_t)size);
        return !out.empty();
    }

    CfbSource*               _src = nullptr;
    uint32_t                 _sectorSize = 512;
    uint32_t                 _miniSectorSize = 64;
    uint32_t                 _miniCutoff = 4096;
    std::vector<uint32_t>    _fat;
    std::vector<uint32_t>    _miniFat;
    std::vector<CfbDirEntry> _entries;
    std::vector<uint8_t>     _miniStream;
};

typedef CfbReader CfbHandleReader;
