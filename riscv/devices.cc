#include "devices.h"

void bus_t::add_device(reg_t addr, abstract_device_t* dev)
{
  devices[-addr] = dev;
}

bool bus_t::load(reg_t addr, size_t len, uint8_t* bytes)
{
  auto it = devices.lower_bound(-addr);
  if (it == devices.end())
    return false;
  return it->second->load(addr - -it->first, len, bytes);
}

bool bus_t::store(reg_t addr, size_t len, const uint8_t* bytes)
{
  auto it = devices.lower_bound(-addr);
  if (it == devices.end())
    return false;
  return it->second->store(addr - -it->first, len, bytes);
}

bool uart_dev_t::load(reg_t addr, size_t len, uint8_t* bytes)
{
  switch(addr) {
    case 0: // UART_REG_TXFIFO
      for (size_t i = 0 ; i < len ; i++) bytes[i] = 0x00;
      return true;
    case 4: // UART_REG_RXFIFO
      // TODO: hook stdin?
      for (size_t i = 0 ; i < len ; i++) bytes[i] = 0xff;
      return true;
    default:
      return false;
  }
}

bool uart_dev_t::store(reg_t addr, size_t len, const uint8_t* bytes)
{
  switch(addr) {
    case 0: // UART_REG_TXFIFO
      if (print) fputc((char)(bytes[0]), stdout);
      return true;
    case 4: // UART_REG_RXFIFO
      return true;
    case 8: // UART_REG_TXCTRL
      return true;
    case 12: // UART_REG_RXCTRL
      return true;
    case 16: // UART_REG_DIV
      return true;
    default:
      return false;
  }
}
