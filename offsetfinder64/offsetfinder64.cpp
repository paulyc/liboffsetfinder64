//
//  offsetfinder64.cpp
//  offsetfinder64
//
//  Created by tihmstar on 10.01.18.
//  Copyright © 2018 tihmstar. All rights reserved.
//

#include "offsetfinder64.hpp"


extern "C"{
#include <stdio.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <string.h>
#include "img4.h"
#include "lzssdec.h"
}

#define info(a ...) ({printf(a),printf("\n");})
#define log(a ...) ({if (dbglog) printf(a),printf("\n");})
#define warning(a ...) ({if (dbglog) printf("[WARNING] "), printf(a),printf("\n");})
#define error(a ...) ({printf("[Error] "),printf(a),printf("\n");})

#define safeFree(ptr) ({if (ptr) free(ptr),ptr=NULL;})

#define reterror(err) throw tihmstar::exception(__LINE__,err)
#define assure(cond) if ((cond) == 0) throw tihmstar::exception(__LINE__, "assure failed")
#define doassure(cond,code) do {if (!(cond)){(code);assure(cond);}} while(0)
#define retassure(cond, err) if ((cond) == 0) throw tihmstar::exception(__LINE__,err)
#define assureclean(cond) do {if (!(cond)){clean();assure(cond);}} while(0)


#ifdef DEBUG
#define OFFSETFINDER64_VERSION_COMMIT_COUNT "debug"
#define OFFSETFINDER64_VERSION_COMMIT_SHA "debug build"
#endif

using namespace std;
using namespace tihmstar;
using namespace patchfinder64;

using segment_t = std::vector<offsetfinder64::text_t>;

#define HAS_BITS(a,b) (((a) & (b)) == (b))
#define _symtab getSymtab()
int decompress_lzss(u_int8_t *dst, u_int8_t *src, u_int32_t srclen);

namespace patchfinder64 {
    class insn;
}


#pragma mark macho external

__attribute__((always_inline)) struct load_command *find_load_command64(struct mach_header_64 *mh, uint32_t lc){
    struct load_command *lcmd = (struct load_command *)(mh + 1);
    for (uint32_t i=0; i<mh->ncmds; i++, lcmd = (struct load_command *)((uint8_t *)lcmd + lcmd->cmdsize)) {
        if (lcmd->cmd == lc)
            return lcmd;
    }
    
    reterror("Failed to find load command "+ to_string(lc));
    return NULL;
}

__attribute__((always_inline)) struct symtab_command *find_symtab_command(struct mach_header_64 *mh){
    return (struct symtab_command *)find_load_command64(mh, LC_SYMTAB);
}

__attribute__((always_inline)) struct dysymtab_command *find_dysymtab_command(struct mach_header_64 *mh){
    return (struct dysymtab_command *)find_load_command64(mh, LC_DYSYMTAB);
}

__attribute__((always_inline)) struct section_64 *find_section(struct segment_command_64 *seg, const char *sectname){
    struct section_64 *sect = (struct section_64 *)(seg + 1);
    for (uint32_t i=0; i<seg->nsects; i++, sect++) {
        if (strcmp(sect->sectname, sectname) == 0)
            return sect;
    }
    reterror("Failed to find section "+ string(sectname));
    return NULL;
}

offsetfinder64::offsetfinder64(const char* filename) : _freeKernel(true),__symtab(NULL){
    struct stat fs = {0};
    int fd = 0;
    char *img4tmp = NULL;
    auto clean =[&]{
        if (fd>0) close(fd);
    };
    assure((fd = open(filename, O_RDONLY)) != -1);
    assureclean(!fstat(fd, &fs));
    assureclean((_kdata = (uint8_t*)malloc( _ksize = fs.st_size)));
    assureclean(read(fd,_kdata,_ksize)==_ksize);
    
    //check if feedfacf, lzss, img4, im4p
    img4tmp = (char*)_kdata;
    if (sequenceHasName(img4tmp, (char*)"IMG4")){
        img4tmp = getElementFromIMG4((char*)_kdata, (char*)"IM4P");
    }
    if (sequenceHasName(img4tmp, (char*)"IM4P")){
        /*extract file from IM4P*/
        char *extractFile = [](char *buf, char **dstBuf)->char*{
            int elems = asn1ElementsInObject(buf);
            if (elems < 4){
                error("not enough elements in SEQUENCE %d\n",elems);
                return NULL;
            }
            
            char *dataTag = asn1ElementAtIndex(buf, 3)+1;
            t_asn1ElemLen dlen = asn1Len(dataTag);
            char *data = dataTag+dlen.sizeBytes;
            
            char *kernel = NULL;
            if ((kernel = tryLZSS(data, (size_t*)&dlen.dataLen))){
                data = kernel;
                printf("lzsscomp detected, uncompressing...\n");
            }
            return kernel;
        }(img4tmp,&extractFile);
        /* done extract file from IM4P*/
        
        free(_kdata);
        _kdata = (uint8_t*)extractFile;
    }
    
    assureclean(*(uint32_t*)_kdata == 0xfeedfacf);
    
    loadSegments(0);
    clean();
}

void offsetfinder64::loadSegments(uint64_t slide){
    printf("getting kernelbase: ");
    _kslide = slide;
    struct mach_header_64 *mh = (struct mach_header_64*)_kdata;
    struct load_command *lcmd = (struct load_command *)(mh + 1);
    for (uint32_t i=0; i<mh->ncmds; i++, lcmd = (struct load_command *)((uint8_t *)lcmd + lcmd->cmdsize)) {
        if (lcmd->cmd == LC_SEGMENT_64){
            struct segment_command_64* seg = (struct segment_command_64*)lcmd;
            _segments.push_back({_kdata+seg->fileoff,seg->filesize, (loc_t)seg->vmaddr, (seg->maxprot & VM_PROT_EXECUTE) !=0});
        }
    }
    
    info("Inited offsetfinder64 %s %s\n",OFFSETFINDER64_VERSION_COMMIT_COUNT, OFFSETFINDER64_VERSION_COMMIT_SHA);
    
}

offsetfinder64::offsetfinder64(void* buf, size_t size, uint64_t slide) : _freeKernel(false),_kdata((uint8_t*)buf),_ksize(size),__symtab(NULL){
    loadSegments(slide);
}



#pragma mark macho offsetfinder
__attribute__((always_inline)) struct symtab_command *offsetfinder64::getSymtab(){
    if (!__symtab)
        __symtab = find_symtab_command((struct mach_header_64 *)_kdata);
    return __symtab;
}

#pragma mark offsetfidner

loc_t offsetfinder64::memmem(const void *little, size_t little_len){
    for (auto seg : _segments) {
        if (loc_t rt = (loc_t)::memmem(seg.map, seg.size, little, little_len)) {
            return rt-seg.map+seg.base+_kslide;
        }
    }
    return 0;
}


offset_t offsetfinder64::find_sym(const char *sym){
    uint8_t *psymtab = _kdata + _symtab->symoff;
    uint8_t *pstrtab = _kdata + _symtab->stroff;

    struct nlist_64 *entry = (struct nlist_64 *)psymtab;
    for (uint32_t i = 0; i < _symtab->nsyms; i++, entry++)
        if (!strcmp(sym, (char*)(pstrtab + entry->n_un.n_strx)))
            return entry->n_value;

    reterror("Failed to find symbol "+string(sym));
    return 0;
}

#pragma mark patchfinder64
namespace tihmstar{
    namespace patchfinder64{
        
        class insn{
            std::pair <loc_t,int> _p;
            std::vector<offsetfinder64::text_t> _segments;
            offset_t _kslide;
            bool _textOnly;
        public:
            insn(segment_t segments, offset_t kslide, loc_t p = 0, bool textOnly = 1) : _segments(segments), _kslide(kslide), _textOnly(textOnly){
                std::sort(_segments.begin(),_segments.end(),[ ]( const offsetfinder64::text_t& lhs, const offsetfinder64::text_t& rhs){
                    return lhs.base < rhs.base;
                });
                if (_textOnly) {
                    _segments.erase(std::remove_if(_segments.begin(), _segments.end(), [](const offsetfinder64::text_t obj){
                        return !obj.isExec;
                    }));
                }
                if (p == 0) {
                    p = _segments.at(0).base;
                }
                for (int i=0; i<_segments.size(); i++){
                    auto seg = _segments[i];
                    if ((loc_t)seg.base <= p && p < (loc_t)seg.base+seg.size){
                        _p = {p,i};
                        return;
                    }
                }
                reterror("initializing insn with out of range location");
            }
            
            insn(const insn &cpy){
                _segments = cpy._segments;
                _kslide = cpy._kslide;
                _p = cpy._p;
                _textOnly = cpy._textOnly;
            }
            
            insn &operator++(){
                _p.first+=4;
                if (_p.first >=_segments[_p.second].base+_segments[_p.second].size){
                    if (_p.second+1 < _segments.size()) {
                        _p.first = _segments[++_p.second].base;
                    }else{
                        _p.first-=4;
                        throw out_of_range("overflow");
                    }
                }
                return *this;
            }
            insn &operator--(){
                _p.first-=4;
                if (_p.first < _segments[_p.second].base){
                    if (_p.second-1 >0) {
                        --_p.second;
                        _p.first = _segments[_p.second].base+_segments[_p.second].size;
                    }else{
                        _p.first+=4;
                        throw out_of_range("underflow");
                    }
                }
                return *this;
            }
            insn operator+(int i){
                insn cpy(*this);
                if (i>0) {
                    while (i--)
                        ++cpy;
                }else{
                    while (i++)
                        --cpy;
                }
                return cpy;
            }
            insn operator-(int i){
                return this->operator+(-i);
            }
            
        public: //helpers
            int64_t signExtend64(uint64_t v, int vSize){
                uint64_t e = (v & 1 << (vSize-1))>>(vSize-1);
                for (int i=vSize; i<64; i++)
                    v |= e << i;
                return v;
            }
            uint64_t pc(){
                return (uint64_t)_p.first + (uint64_t)_kslide;
            }
            uint32_t value(){
                return (*(uint32_t*)(loc_t)(*this));
            }
            
        public: //static type determinition
            static bool is_adrp(uint32_t i){
                return ((i>>24) % (1<<5)) == 0b10000 && (i>>31);
            }
            static bool is_add(uint32_t i){
                return ((i>>24) % (1<<5)) == 0b10001;
            }
            static bool is_bl(uint32_t i){
                return (i>>26) == 0b100101;
            }
            static bool is_cbz(uint32_t i){
                return ((i>>24) % (1<<7)) == 0b0110100;
            }
            static bool is_ret(uint32_t i){
                return ((0b11111 << 5) | i) == 0b11010110010111110000001111100000;
            }
            static bool is_tbnz(uint32_t i){
                return ((i>>24) % (1<<7)) == 0b0110111;
            }
            static bool is_ldr(uint32_t i){
                return ((i>>21) | 0b010000000000) == 0b11111000011;
            }
            
        public: //type
            enum type{
                unknown,
                adrp,
                bl,
                cbz,
                ret,
                tbnz,
                add
            };
            enum subtype{
                st_general,
                st_register,
                st_immediate,
                st_literal
            };
            enum supertype{
                sut_general,
                sut_branch
            };
            type type(){
                if (is_adrp(value()))
                    return adrp;
                else if (is_add(value()))
                    return add;
                else if (is_bl(value()))
                    return bl;
                else if (is_cbz(value()))
                    return cbz;
                else if (is_ret(value()))
                    return ret;
                else if (is_tbnz(value()))
                    return tbnz;
                return unknown;
            }
            subtype subtype(){
//                if (is_ldr(value)) {
//                    
//                }
                
                return st_general;
            }
            supertype supertype(){
                switch (type()) {
                    case bl:
                    case cbz:
                    case tbnz:
                        return sut_branch;
                        
                    default:
                        return sut_general;
                }
            }
            uint64_t imm(){
                switch (type()) {
                    case unknown:
                        reterror("can't get imm value of unknown instruction");
                        break;
                    case adrp:
                        return ((pc()>>12)<<12) + signExtend64(((((value() % (1<<24))>>5)<<2) | ((value()>>29) % (1<<2)))<<12,32);
                    case add:
                        return ((value()>>10) % (1<<12)) << (((value()>>22)&1) * 12);
                    case bl:
                        return signExtend64(value() % (1<<26), 25); //untested
                    case cbz:
                        return signExtend64((value() >> 5) % (1<<19), 19); //untested
                    case tbnz:
                        return signExtend64((value() >> 5) % (1<<19), 19); //untested
                        
                    default:
                        reterror("failed to get imm value");
                        break;
                }
                return 0;
            }
            uint8_t rd(){
                switch (type()) {
                    case unknown:
                        reterror("can't get rd of unknown instruction");
                        break;
                    case adrp:
                        return (value() % (1<<5));
                        
                    case add:
                        return (value() % (1<<5));
                        
                    default:
                        reterror("failed to get rd");
                        break;
                }
            }
            uint8_t rn(){
                switch (type()) {
                    case unknown:
                        reterror("can't get rn of unknown instruction");
                        break;
                    case add:
                        return ((value() >>5) % (1<<5));
                    case ret:
                        return ((value() >>5) % (1<<5));
                        
                    default:
                        reterror("failed to get rn");
                        break;
                }
            }
            uint8_t rt(){
                switch (type()) {
                    case unknown:
                        reterror("can't get rt of unknown instruction");
                        break;
                    case cbz:
                        return (value() % (1<<5));
                    case tbnz:
                        return (value() % (1<<5));
                        
                    default:
                        reterror("failed to get rt");
                        break;
                }
            }
        public: //cast operators
            operator loc_t(){
                return (loc_t)(_p.first - _segments[_p.second].base + _segments[_p.second].map);
            }
            operator enum type(){
                return type();
            }
        };
        
        loc_t find_literal_ref(segment_t segemts, offset_t kslide, loc_t pos){
            insn adrp(segemts,kslide);
            
            uint8_t rd = 0xff;
            uint64_t imm = 0;
            try {
                while (1){
                    if (adrp.type() == insn::adrp) {
                        rd = adrp.rd();
                        imm = adrp.imm();
                    }else if (adrp.type() == insn::add && rd == adrp.rd()){
                        if (imm + adrp.imm() == (uint64_t)pos)
                            return (loc_t)adrp.pc();
                    }
                    ++adrp;
                }
                
                
            } catch (std::out_of_range &e) {
                return 0;
            }
            return 0;
        }
        loc_t find_rel_branch_source(insn bdst, bool searchUp){
            insn bsrc(bdst);
            
            while (true) {
                if (searchUp)
                    while ((--bsrc).supertype() != insn::sut_branch);
                else
                    while ((++bsrc).supertype() != insn::sut_branch);
                
                if (bsrc.imm()*4 + bsrc.pc()  == bdst.pc()) {
                    return (loc_t)bsrc.pc();
                }
            }
            return 0;
        }

    };
};

patch offsetfinder64::find_sandbox_patch(){
    loc_t str = memmem("process-exec denied while updating label", sizeof("process-exec denied while updating label")-1);
    retassure(str, "Failed to find str");

    loc_t ref = find_literal_ref(_segments, _kslide, str);
    retassure(ref, "literal ref to str");

    insn bdst(_segments, _kslide, ref);
    for (int i=0; i<4; i++) {
        while (--bdst != insn::bl){
        }
    }
    --bdst;
    
    loc_t cbz = find_rel_branch_source(bdst, true);
    
    constexpr char nop[] = "\x1F\x20\x03\xD5";
    return patch(cbz, nop, sizeof(nop)-1);
}


patch offsetfinder64::find_amfi_substrate_patch(){
    loc_t str = memmem("AMFI: hook..execve() killing pid %u: %s", sizeof("AMFI: hook..execve() killing pid %u: %s")-1);
    retassure(str, "Failed to find str");

    loc_t ref = find_literal_ref(_segments, _kslide, str);
    retassure(ref, "literal ref to str");

    insn funcend(_segments, _kslide, ref);
    while (++funcend != insn::ret);
    
    insn tbnz(funcend);
    while (--tbnz != insn::tbnz);
    
    constexpr char mypatch[] = "\x1F\x20\x03\xD5\x00\x78\x16\x12\x1F\x20\x03\xD5\x00\x00\x80\x52\xE9\x01\x80\x52";
    return {(loc_t)tbnz.pc(),mypatch,sizeof(mypatch)-1};
}

patch offsetfinder64::find_cs_enforcement_disable_amfi(){
    loc_t str = memmem("csflags", sizeof("csflags"));
    retassure(str, "Failed to find str");
    
    loc_t ref = find_literal_ref(_segments, _kslide, str);
    retassure(ref, "literal ref to str");

    insn cbz(_segments, _kslide, ref);
    while (--cbz != insn::cbz);
    
    insn ret(cbz);
    while (++ret != insn::ret);

    int anz = static_cast<int>((ret.pc()-cbz.pc())/4 +1);
    
    constexpr char nop[] = "\x1F\x20\x03\xD5";
    char mypatch[anz*4];
    for (int i=0; i<anz; i++) {
        ((uint32_t*)mypatch)[i] = *(uint32_t*)nop;
    }

    return {(loc_t)cbz.pc(),mypatch,static_cast<size_t>(anz*4)};
}

patch offsetfinder64::find_i_can_has_debugger_patch_off(){
    loc_t str = memmem("Darwin Kernel", sizeof("Darwin Kernel")-1);
    retassure(str, "Failed to find str");
    
    str -=4;
    
    return {str,"\x01",1};
}

patch offsetfinder64::find_amfi_patch_offsets(){
    loc_t str = memmem("int _validateCodeDirectoryHashInDaemon", sizeof("int _validateCodeDirectoryHashInDaemon")-1);
    retassure(str, "Failed to find str");
    
    loc_t ref = find_literal_ref(_segments, _kslide, str);
    retassure(ref, "literal ref to str");

    insn bl_amfi_memcp(_segments, _kslide, ref);

    
    while (1) {
        while (++bl_amfi_memcp != insn::bl);
        printf("%p %p\n",bl_amfi_memcp.pc(),bl_amfi_memcp.imm()*4+bl_amfi_memcp.pc());
        insn fdst(_segments, _kslide, (loc_t)(bl_amfi_memcp.imm()*4+bl_amfi_memcp.pc()));
        if (fdst == insn::adrp) {
            printf("maybe");
        }
        
        printf("");
    }
    
    
    
    return {0,0,0};
}



offsetfinder64::~offsetfinder64(){
    if (_freeKernel) safeFree(_kdata);
}










//