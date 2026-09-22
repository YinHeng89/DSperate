// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#include "core/cart/rom_source.h"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace ds::cart {

static constexpr u64 MAX_ROM = 512ull << 20;   // a DS card tops out at 512 MB

RomSource::~RomSource() {
  if (map_) munmap(map_, map_len_);
}

const u8* RomSource::ff_page() {
  static const u8* p = [] { static u8 page[PAGE]; std::memset(page, 0xFF, PAGE); return page; }();
  return p;
}

void RomSource::finish() {
  u32 m = PAGE; while (m < size_) m <<= 1;
  mask_ = m - 1;
  const u32 rem = size_ & (PAGE - 1);
  if (rem) {
    // Real bytes then 0xFF, so nothing reads the mapping past EOF.
    tail_.assign(PAGE, 0xFF);
    std::memcpy(tail_.data(), data_ + (size_ - rem), rem);
  }
}

std::unique_ptr<RomSource> RomSource::from_memory(std::vector<u8> bytes) {
  std::unique_ptr<RomSource> s(new RomSource);
  s->owned_ = std::move(bytes);
  s->data_ = s->owned_.data();
  s->size_ = static_cast<u32>(s->owned_.size());
  s->finish();
  return s;
}

std::unique_ptr<RomSource> RomSource::map_file(const std::string& path, u64 offset, u64 size,
                                               std::string& err) {
  err.clear();
  const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) { err = std::strerror(errno); return nullptr; }
  struct stat st{};
  if (fstat(fd, &st) != 0) { err = std::strerror(errno); close(fd); return nullptr; }
  const u64 file = static_cast<u64>(st.st_size);
  if (offset > file) { err = "range starts past the end of the file"; close(fd); return nullptr; }
  if (size == 0) size = file - offset;
  if (size > file - offset) { err = "range runs past the end of the file"; close(fd); return nullptr; }
  if (size > MAX_ROM) { err = "larger than any DS card"; close(fd); return nullptr; }
  if (size == 0) { err = "empty"; close(fd); return nullptr; }

  const long ps = sysconf(_SC_PAGESIZE);
  const u64 align = static_cast<u64>(ps > 0 ? ps : 4096);
  const u64 base = offset & ~(align - 1);
  const size_t len = static_cast<size_t>(offset - base + size);
  void* m = mmap(nullptr, len, PROT_READ, MAP_PRIVATE, fd, static_cast<off_t>(base));
  close(fd);
  if (m == MAP_FAILED) { err = std::string("mmap: ") + std::strerror(errno); return nullptr; }

  std::unique_ptr<RomSource> s(new RomSource);
  s->map_ = m; s->map_len_ = len;
  s->data_ = static_cast<const u8*>(m) + (offset - base);
  s->size_ = static_cast<u32>(size);
  s->finish();
  return s;
}

u8* RomSource::patch(u32 addr) {
  const u32 p = addr & mask_ & ~(PAGE - 1);
  for (Patch& o : overlay_) if (o.base == p) return o.bytes.data();
  Patch o; o.base = p; o.bytes.assign(page(p), page(p) + PAGE);
  overlay_.push_back(std::move(o));
  return overlay_.back().bytes.data();
}

void RomSource::read(u32 addr, u8* dst, u32 n) const {
  while (n) {
    const u8* pg = page(addr);
    const u32 off = addr & (PAGE - 1);
    const u32 take = n < PAGE - off ? n : PAGE - off;
    std::memcpy(dst, pg + off, take);
    dst += take; addr += take; n -= take;
  }
}

u32 RomSource::read_unpatched(u32 addr, u8* dst, u32 n) const {
  // Deliberately linear, not wrapping at the mask: a wrapping request counts
  // as unavailable rather than reading from the front of the image.
  const u32 start = addr & mask_;
  u32 have = 0;
  if (start < size_) have = n < size_ - start ? n : size_ - start;

  while (n) {
    const u8* pg = page_unpatched(addr & mask_ & ~(PAGE - 1));
    const u32 off = addr & (PAGE - 1);
    const u32 take = n < PAGE - off ? n : PAGE - off;
    std::memcpy(dst, pg + off, take);
    dst += take; addr += take; n -= take;
  }
  return have;
}

} // namespace ds::cart
