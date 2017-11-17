// See LICENSE for license details.

#ifndef _RISCV_MMU_H
#define _RISCV_MMU_H

#include "decode.h"
#include "trap.h"
#include "common.h"
#include "config.h"
#include "sim.h"
#include "processor.h"
#include "memtracer.h"
#include <stdlib.h>
#include <vector>
#include <array>
#include <unordered_map>
#include <deque>

// virtual memory configuration
#define PGSHIFT 12
const reg_t PGSIZE = 1 << PGSHIFT;
const reg_t PGMASK = ~(PGSIZE-1);

struct insn_fetch_t
{
  insn_func_t func;
  insn_t insn;
};

struct icache_entry_t {
  reg_t tag;
  reg_t pad;
  insn_fetch_t data;
};

class trigger_matched_t
{
  public:
    trigger_matched_t(int index,
        trigger_operation_t operation, reg_t address, reg_t data) :
      index(index), operation(operation), address(address), data(data) {}

    int index;
    trigger_operation_t operation;
    reg_t address;
    reg_t data;
};

// Dongggyu: TLB models
struct tlb_t {
  std::array<reg_t, 256> meta;
  std::array<reg_t, 256> tags;
  std::unordered_map<reg_t, size_t> tag_map;
  tlb_t() {
    std::fill(meta.begin(), meta.end(), 0);
    std::fill(tags.begin(), tags.end(), -1ULL);
  }
  tlb_t(const tlb_t& tlb) {
    copy(tlb);
  }
  tlb_t& operator=(const tlb_t& tlb) {
    if (this != &tlb) copy(tlb);
    return *this;
  }
private:
  void copy(const tlb_t& tlb) {
    std::copy(tlb.meta.begin(), tlb.meta.end(), meta.begin());
    std::copy(tlb.tags.begin(), tlb.tags.end(), tags.begin());
    tag_map = tlb.tag_map;
  }
};

enum tlb_type_t { ITLB, DTLB };

// this class implements a processor's port into the virtual memory system.
// an MMU and instruction cache are maintained for simulator performance.
class mmu_t
{
public:
  mmu_t(sim_t* sim, processor_t* proc);
  ~mmu_t();

  // template for functions that load an aligned value from memory
  #define load_func(type) \
    inline type##_t load_##type(reg_t addr) { \
      if (addr & (sizeof(type##_t)-1)) \
        throw trap_load_address_misaligned(addr); \
      reg_t vpn = addr >> PGSHIFT; \
      if (likely(timewarp)) check_permission(addr, LOAD); \
      if (likely(tlb_load_tag[vpn % TLB_ENTRIES] == vpn)) \
        return *(type##_t*)(tlb_data[vpn % TLB_ENTRIES] + addr); \
      if (unlikely(tlb_load_tag[vpn % TLB_ENTRIES] == (vpn | TLB_CHECK_TRIGGERS))) { \
        type##_t data = *(type##_t*)(tlb_data[vpn % TLB_ENTRIES] + addr); \
        if (!matched_trigger) { \
          matched_trigger = trigger_exception(OPERATION_LOAD, addr, data); \
          if (matched_trigger) \
            throw *matched_trigger; \
        } \
        return data; \
      } \
      type##_t res; \
      load_slow_path(addr, sizeof(type##_t), (uint8_t*)&res); \
      return res; \
    }

  // load value from memory at aligned address; zero extend to register width
  load_func(uint8)
  load_func(uint16)
  load_func(uint32)
  load_func(uint64)

  // load value from memory at aligned address; sign extend to register width
  load_func(int8)
  load_func(int16)
  load_func(int32)
  load_func(int64)

  // template for functions that store an aligned value to memory
  #define store_func(type) \
    void store_##type(reg_t addr, type##_t val) { \
      if (addr & (sizeof(type##_t)-1)) \
        throw trap_store_address_misaligned(addr); \
      reg_t vpn = addr >> PGSHIFT; \
      if (likely(timewarp)) check_permission(addr, STORE); \
      if (likely(tlb_store_tag[vpn % TLB_ENTRIES] == vpn)) { \
        char* data = tlb_data[vpn % TLB_ENTRIES] + addr; \
	if (timewarp) \
          record(sizeof(type##_t), sim->mem_to_addr(data), *(type##_t*)(data)); \
        *(type##_t*)(data) = val; \
      } else if (unlikely(tlb_store_tag[vpn % TLB_ENTRIES] == (vpn | TLB_CHECK_TRIGGERS))) { \
        if (!matched_trigger) { \
          matched_trigger = trigger_exception(OPERATION_STORE, addr, val); \
          if (matched_trigger) \
            throw *matched_trigger; \
        } \
        char* data = tlb_data[vpn % TLB_ENTRIES] + addr; \
	if (timewarp) \
          record(sizeof(type##_t), sim->mem_to_addr(data), *(type##_t*)(data)); \
        *(type##_t*)(data) = val; \
      } \
      else \
        store_slow_path(addr, sizeof(type##_t), (const uint8_t*)&val); \
    }

  // template for functions that perform an atomic memory operation
  #define amo_func(type) \
    template<typename op> \
    type##_t amo_##type(reg_t addr, op f) { \
      if (addr & (sizeof(type##_t)-1)) \
        throw trap_store_address_misaligned(addr); \
      try { \
        auto lhs = load_##type(addr); \
        store_##type(addr, f(lhs)); \
        return lhs; \
      } catch (trap_load_access_fault& t) { \
        /* AMO faults should be reported as store faults */ \
        throw trap_store_access_fault(t.get_badaddr()); \
      } \
    }

  // store value to memory at aligned address
  store_func(uint8)
  store_func(uint16)
  store_func(uint32)
  store_func(uint64)

  // perform an atomic memory operation at an aligned address
  amo_func(uint32)
  amo_func(uint64)

  static const reg_t ICACHE_ENTRIES = 1024;

  inline size_t icache_index(reg_t addr)
  {
    return (addr / PC_ALIGN) % ICACHE_ENTRIES;
  }

  inline icache_entry_t* refill_icache(reg_t addr, icache_entry_t* entry)
  {
    const uint16_t* iaddr = translate_insn_addr(addr);
    insn_bits_t insn = *iaddr;
    int length = insn_length(insn);

    if (likely(length == 4)) {
      insn |= (insn_bits_t)*(const int16_t*)translate_insn_addr(addr + 2) << 16;
    } else if (length == 2) {
      insn = (int16_t)insn;
    } else if (length == 6) {
      insn |= (insn_bits_t)*(const int16_t*)translate_insn_addr(addr + 4) << 32;
      insn |= (insn_bits_t)*(const uint16_t*)translate_insn_addr(addr + 2) << 16;
    } else {
      static_assert(sizeof(insn_bits_t) == 8, "insn_bits_t must be uint64_t");
      insn |= (insn_bits_t)*(const int16_t*)translate_insn_addr(addr + 6) << 48;
      insn |= (insn_bits_t)*(const uint16_t*)translate_insn_addr(addr + 4) << 32;
      insn |= (insn_bits_t)*(const uint16_t*)translate_insn_addr(addr + 2) << 16;
    }

    insn_fetch_t fetch = {proc->decode_insn(insn), insn};
    entry->tag = addr;
    entry->data = fetch;

    reg_t paddr = sim->mem_to_addr((char*)iaddr);
    if (tracer.interested_in_range(paddr, paddr + 1, FETCH)) {
      entry->tag = -1;
      tracer.trace(paddr, length, FETCH);
    }
    return entry;
  }

  inline icache_entry_t* access_icache(reg_t addr)
  {
    icache_entry_t* entry = &icache[icache_index(addr)];
    if (likely(entry->tag == addr))
      return entry;
    return refill_icache(addr, entry);
  }

  inline insn_fetch_t load_insn(reg_t addr)
  {
    icache_entry_t entry;
    return refill_icache(addr, &entry)->data;
  }

  void flush_tlb();
  void flush_icache();

  void register_memtracer(memtracer_t*);

  // By Donggyu
  void set_timewarp(bool value) {
    timewarp = value;
  }
  void set_permission(size_t addr, reg_t tag, reg_t meta, uint8_t tpe);
  void flush_permission();

private:
  sim_t* sim;
  processor_t* proc;
  memtracer_list_t tracer;
  uint16_t fetch_temp;

  // By Donggyu
  tlb_t itlb, dtlb;

  // implement an instruction cache for simulator performance
  icache_entry_t icache[ICACHE_ENTRIES];

  // implement a TLB for simulator performance
  static const reg_t TLB_ENTRIES = 256;
  // If a TLB tag has TLB_CHECK_TRIGGERS set, then the MMU must check for a
  // trigger match before completing an access.
  static const reg_t TLB_CHECK_TRIGGERS = reg_t(1) << 63;
  char* tlb_data[TLB_ENTRIES];
  reg_t tlb_insn_tag[TLB_ENTRIES];
  reg_t tlb_load_tag[TLB_ENTRIES];
  reg_t tlb_store_tag[TLB_ENTRIES];

  // finish translation on a TLB miss and update the TLB
  void refill_tlb(reg_t vaddr, reg_t paddr, access_type type);
  const char* fill_from_mmio(reg_t vaddr, reg_t paddr);

  // perform a page table walk for a given VA; set referenced/dirty bits
  reg_t walk(reg_t addr, access_type type, reg_t prv);

  // handle uncommon cases: TLB misses, page faults, MMIO
  const uint16_t* fetch_slow_path(reg_t addr);
  void load_slow_path(reg_t addr, reg_t len, uint8_t* bytes);
  void store_slow_path(reg_t addr, reg_t len, const uint8_t* bytes);
  reg_t translate(reg_t addr, access_type type);

  inline void check_permission(reg_t vaddr, access_type type) {
return;
    reg_t mode = proc->state.prv;
    if (type != FETCH) {
      if (!proc->state.dcsr.cause && get_field(proc->state.mstatus, MSTATUS_MPRV))
        mode = get_field(proc->state.mstatus, MSTATUS_MPP);
    }
    if (get_field(proc->state.mstatus, MSTATUS_VM) == VM_MBARE)
      mode = PRV_M;
    if (mode == PRV_M) return;

    bool supervisor = mode == PRV_S;
    bool pum = get_field(proc->state.mstatus, MSTATUS_PUM);
    bool mxr = get_field(proc->state.mstatus, MSTATUS_MXR);
    tlb_t* tlb = (type == FETCH) ? &itlb : &dtlb;
    reg_t tag = vaddr >> PGSHIFT;
    switch (get_field(proc->get_state()->mstatus, MSTATUS_VM)) {
      case VM_SV32: tag &= ((1ULL << 20) - 1);
      case VM_SV39: tag &= ((1ULL << 27) - 1);
      case VM_SV48: tag &= ((1ULL << 36) - 1);
    }
    auto match = tlb->tag_map.find(tag);
    if (match == tlb->tag_map.end()) return;

    size_t addr = match->second;
    reg_t meta = tlb->meta[addr];
    bool no_priv = (meta & PTE_U) ? supervisor && pum : !supervisor;
    bool no_valid = !(meta & PTE_V) || (!(meta & PTE_R) && (meta & PTE_W));
    switch(type) {
      case FETCH: {
        bool xcpt_if = no_priv || no_valid ||
          !(meta & PTE_X);
        if (xcpt_if) {
          throw trap_instruction_access_fault(vaddr);
        }
        break;
      }
      case LOAD: {
        bool xcpt_ld = no_priv || no_valid ||
          (!(meta & PTE_R) && !(mxr && (meta & PTE_X)));
        if (xcpt_ld) {
          throw trap_load_access_fault(vaddr);
        }
        break;
      }
      case STORE: {
        // miss if dirty is off
        bool miss = !no_priv && !no_valid &&
          (meta & PTE_W) && !(meta & PTE_D);
        if (miss) return;
        bool xcpt_st = no_priv || no_valid ||
          (!((meta & PTE_R) && (meta & PTE_W)));
        if (xcpt_st) {
          throw trap_store_access_fault(vaddr);
        }
        break;
      }
    }
  } 

  // ITLB lookup
  inline const uint16_t* translate_insn_addr(reg_t addr) {
    reg_t vpn = addr >> PGSHIFT;
    if (likely(timewarp)) check_permission(addr, FETCH);
    if (likely(tlb_insn_tag[vpn % TLB_ENTRIES] == vpn))
      return (uint16_t*)(tlb_data[vpn % TLB_ENTRIES] + addr);
    if (unlikely(tlb_insn_tag[vpn % TLB_ENTRIES] == (vpn | TLB_CHECK_TRIGGERS))) {
      uint16_t* ptr = (uint16_t*)(tlb_data[vpn % TLB_ENTRIES] + addr);
      int match = proc->trigger_match(OPERATION_EXECUTE, addr, *ptr);
      if (match >= 0)
        throw trigger_matched_t(match, OPERATION_EXECUTE, addr, *ptr);
      return ptr;
    }
    return fetch_slow_path(addr);
  }

  inline trigger_matched_t *trigger_exception(trigger_operation_t operation,
      reg_t address, reg_t data)
  {
    if (!proc) {
      return NULL;
    }
    int match = proc->trigger_match(operation, address, data);
    if (match == -1)
      return NULL;
    if (proc->state.mcontrol[match].timing == 0) {
      throw trigger_matched_t(match, operation, address, data);
    }
    return new trigger_matched_t(match, operation, address, data);
  }

  bool check_triggers_fetch;
  bool check_triggers_load;
  bool check_triggers_store;
  // The exception describing a matched trigger, or NULL.
  trigger_matched_t *matched_trigger;

  friend class processor_t;

  // Time Warp
  bool timewarp;
  struct trace_t {
    trace_t(size_t len, reg_t addr, reg_t data):
      len(len), addr(addr), data(data) { }
    size_t len;
    reg_t addr;
    reg_t data;
  };
  std::deque<uint64_t> tlb_clks;
  std::deque<tlb_t> itlbs;
  std::deque<tlb_t> dtlbs;
  std::deque<uint64_t> trace_clks;
  std::deque<trace_t> traces;

  uint64_t timestamp() { return proc->timestamp; }
  void record(size_t len, reg_t addr, reg_t data);
  void snapshot(uint64_t timestamp);
  void rollback(uint64_t timestamp);
  void collect_fossils(uint64_t gvt);
};

#endif
