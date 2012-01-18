#include <elf.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>

#include <string>
#include <vector>

void fatal_impl(const char* file, int line, const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  printf("%s:%d: ", file, line);
  vprintf(fmt, ap);
  printf("\n");
  exit(1);
}

#define fatal(fmt, args...) fatal_impl(__FILE__, __LINE__, fmt, ##args)

struct Stream {
  Stream(uint8_t* data, int len) : data_(data), len_(len), ofs_(0) {}

  bool read(void* out, int count) {
    if (ofs_ + count >= len_) {
      ofs_ = len_;
      return false;
    }
    if (out)
      memcpy(out, data_ + ofs_, count);
    ofs_ += count;
    return true;
  }

  bool read_uint8(uint8_t* out) { return read(out, 1); }
  bool read_int8(int8_t* out) { return read(out, 1); }
  bool read_uint16(uint16_t* out) { return read(out, 2); }
  bool read_uint32(uint32_t* out) { return read(out, 4); }
  bool read_uint64(uint64_t* out) { return read(out, 8); }

  bool read_str(std::string* str) {
    uint8_t c;
    for (;;) {
      if (!read_uint8(&c))
        return false;
      if (c == 0)
        break;
      str->push_back(c);
    }
    return true;
  }

  bool read_uleb128(uint64_t* out) {
    uint64_t value = 0;
    int shift = 0;
    uint8_t b;
    for (;;) {
      if (!read_uint8(&b))
        return false;
      value |= ((uint64_t)b & 0x7F) << shift;
      if ((b & 0x80) == 0)
        break;
      shift += 7;
    }
    if (out)
      *out = value;
    return true;
  }

  bool read_sleb128(int64_t* out) {
    int64_t value = 0;
    int shift = 0;
    uint8_t b;
    for (;;) {
      if (!read_uint8(&b))
        return false;
      value |= ((uint64_t)b & 0x7F) << shift;
      shift += 7;
      if ((b & 0x80) == 0)
        break;
    }
    if (shift < 64 && (b & 0x40))
      value |= -(1 << shift);
    if (out)
      *out = value;
    return true;
  }

  uint8_t* data_;
  int len_;
  int ofs_;
};

#define check(x) if (!(x)) { fatal("check %s failed", #x); }

void run_program(uint8_t* data, int len) {
  // section 7.4 to find initial length field size
  // 32 bits on dwarf32, 96 on dwarf64

  Stream in(data, len);

  uint32_t unit_length;
  check(in.read_uint32(&unit_length));
  if (unit_length == 0xFFFFFFFF)
    fatal("dwarf64 unimplemented");
  uint16_t version;
  check(in.read_uint16(&version));
  uint32_t header_length;
  check(in.read_uint32(&header_length));
  uint8_t minimum_instruction_length;
  check(in.read_uint8(&minimum_instruction_length));

  uint8_t default_is_stmt;
  check(in.read_uint8(&default_is_stmt));
  int8_t line_base;
  check(in.read_int8(&line_base));
  uint8_t line_range;
  check(in.read_uint8(&line_range));
  printf("base %d %d\n", line_base, line_range);
  uint8_t opcode_base;
  check(in.read_uint8(&opcode_base));
  uint8_t* opcode_lengths = new uint8_t[opcode_base+1];
  for (int i = 1; i < opcode_base; i++) {
    check(in.read_uint8(&opcode_lengths[i]));
    printf("op %d %d\n", i, opcode_lengths[i]);
  }

  for (;;) {
    std::string path;
    check(in.read_str(&path));
    if (path.empty())
      break;
    printf("%s\n", path.c_str());
  }

  printf("files:\n");
  std::vector<std::string> files;
  files.push_back("who makes 1-indexed arrays, anyway?");
  for (;;) {
    std::string file;
    check(in.read_str(&file));
    if (file.empty())
      break;
    files.push_back(file);
    uint64_t dir_index = 0, mtime = 0, file_length = 0;
    check(in.read_uleb128(&dir_index));
    check(in.read_uleb128(&mtime));
    check(in.read_uleb128(&file_length));
    printf("%s %d %d %d\n", file.c_str(), (int)dir_index, (int)mtime, (int)file_length);
  }

  /*uint8_t c;
  for (int i = 0; i < 32; i++) {
    check(in.read_uint8(&c));
    printf("%d 0x%02x %c\n", i, (int)c, (int)c);
    }*/

  struct Registers {
    uint64_t address;
    int file;
    int line;
    int column;
    bool is_stmt;
    bool basic_block;
    bool end_sequence;
    bool prologue_end;
    bool epilogue_begin;
    int isa;
    void reset(bool default_is_stmt) {
      address = 0;
      file = 1;
      line = 1;
      column = 0;
      is_stmt = default_is_stmt;
      basic_block = false;
      end_sequence = false;
      prologue_end = false;
      epilogue_begin = false;
      isa = 0;
    }
  };
  Registers regs;
  regs.reset(default_is_stmt);

  for (;;) {
    uint8_t op;
    check(in.read_uint8(&op));
    switch (op) {
    case 0x0: { // extended
      check(in.read_uleb128(NULL));  // length
      check(in.read_uint8(&op));
      uint64_t addr;
      switch (op) {
      case 0x01:  // DW_LNE_end_sequence
        printf("end sequence\n");
        regs.end_sequence = true;
        regs.reset(default_is_stmt);
        break;
      case 0x02:  // DW_LNE_set_address
        check(in.read_uint64(&addr));
        printf("set addr 0x%llx\n", (unsigned long long)addr);
        regs.address = addr;
        break;
      case 0x03:  // DW_LNE_define_file
      case 0x80:  // DW_LNE_lo_user
      case 0xff:  // DW_LNE_hi_user
      default:
        fatal("unhandled extended op 0x%x\n", op);
      }
      break;
    }

    case 0x1:  // DW_LNS_copy
      printf("copy\n");
      regs.basic_block = false;
      regs.prologue_end = false;
      regs.epilogue_begin = false;
      break;

    case 0x2: {  // DW_LNS_advance_pc
      uint64_t delta;
      check(in.read_uleb128(&delta));
      delta *= minimum_instruction_length;
      regs.address += delta;
      printf("advance pc %d => %llx\n", (int)delta, (long long)regs.address);
      break;
    }

    case 0x3: {  // DW_LNS_advance_line
      int64_t delta;
      check(in.read_sleb128(&delta));
      regs.line += delta;
      printf("advance line %d => %d\n", (int)delta, (int)regs.line);
      break;
    }

    case 0x4: { // DW_LNS_set_file
      uint64_t file;
      check(in.read_uleb128(&file));
      printf("file %d %s\n", (int)file, files[file].c_str());
      regs.file = file;
      break;
    }

    case 0x5: { // DW_LNS_set_column
      fatal("unimpl\n");
      break;
    }

    case 0x6: { // DW_LNS_negate_stmt
      fatal("unimpl\n");
      break;
    }

    case 0x7: { // DW_LNS_set_basic_block
      fatal("unimpl\n");
      break;
    }

    case 0x8: { // DW_LNS_const_add_pc
      int adjusted_opcode = 255 - opcode_base;
      int address_increment =
        (adjusted_opcode / line_range) * minimum_instruction_length;
      regs.address += address_increment;
      printf("add pc %d => %llx\n", address_increment, (long long)regs.address);
      break;
    }

    case 0x9: { // DW_LNS_fixed_advance_pc
      fatal("unimpl\n");
      break;
    }
      /* dwarf3
DW_LNS_set_prologue_end ‡  0x0a
DW_LNS_set_epilogue_begin ‡  0x0b
DW_LNS_set_isa ‡  0x0c
      */

    default: {
      int adjusted_opcode = op - opcode_base;
      printf("special op 0x%x  ", adjusted_opcode);
      int address_increment =
        (adjusted_opcode / line_range) * minimum_instruction_length;
      int line_increment = line_base + (adjusted_opcode % line_range);
      regs.basic_block = false;
      regs.prologue_end = false;
      regs.epilogue_begin = false;
      regs.address += address_increment;
      regs.line += line_increment;
      printf("addr += %d => %llx, line += %d => %d\n",
             address_increment, (long long)regs.address,
             line_increment, (int)regs.line);
    }
    }
  }


}

int main(int argc, char* argv[]) {
  const char* filename = "maddr";
  int fd = open(filename, O_RDONLY);
  if (fd < 0)
    fatal("open(%s): %s", filename, strerror(errno));

  struct stat st;
  if (fstat(fd, &st) < 0)
    fatal("fstat(%s): %s", filename, strerror(errno));

  uint8_t* data = (uint8_t*)mmap(NULL, st.st_size,
                                 PROT_READ, MAP_PRIVATE,
                                 fd, 0);
  if (!data)
    fatal("mmap(%d): %s", fd, strerror(errno));

  Elf64_Ehdr* header = (Elf64_Ehdr*)data;
  if (memcmp(header->e_ident, ELFMAG, SELFMAG) != 0)
    fatal("%s: bad magic", filename);

  int eclass = header->e_ident[EI_CLASS];
  if (eclass != ELFCLASS64)
    fatal("%s: bad elf class %d", filename, eclass);

  int edata = header->e_ident[EI_DATA];
  if (edata != ELFDATA2LSB)
    fatal("%s: bad elf data format %d", filename, edata);

  int eversion = header->e_ident[EI_VERSION];
  if (eversion != EV_CURRENT)
    fatal("%s: bad elf version %d", filename, eversion);

  /*int eabi = header->e_ident[EI_OSABI];
  if (eabi != ELFOSABI_LINUX)
    fatal("%s: bad elf abi %d", filename, eabi);*/

  Elf64_Shdr* sheaders = (Elf64_Shdr*)(data + header->e_shoff);
  int sheader_count = header->e_shnum;

  Elf64_Shdr* sheader_names = &sheaders[header->e_shstrndx];
  uint8_t* names = &data[sheader_names->sh_offset];

  Elf64_Shdr* shdr_lines = NULL;

  for (int i = 0; i < sheader_count; i++) {
    Elf64_Shdr* shdr = &sheaders[i];
    uint8_t* name = names + shdr->sh_name;
    if (strcmp((char*)name, ".debug_line") == 0) {
      shdr_lines = shdr;
      break;
    }
    printf("header %d name %s\n", i, name);
  }

  if (!shdr_lines)
    fatal("couldn't find .debug_line");

  run_program(&data[shdr_lines->sh_offset], shdr_lines->sh_size);

  return 0;
}
