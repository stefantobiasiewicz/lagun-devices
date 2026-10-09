#include "mcp23017.h"

/* IOCON.BANK = 0: the A and B registers of a pair sit next to each other, so the port
 * is added to the A address. */
#define REG_IODIR 0x00u
#define REG_OLAT  0x14u

/** A two-byte write at 400 kHz takes ~60 µs; 10 ms is plenty. Not more: the write runs
 * on the way to the node's answer, and a chip that does not answer would hold that
 * answer for the whole timeout — at 100 ms, exactly the hub's own, after which it repeats
 * the command (zadania/zrobione/BUG-02.md). */
#define TIMEOUT_MS 10

static esp_err_t write_reg(mcp23017_t *m, uint8_t reg, uint8_t value)
{
    uint8_t frame[2] = {reg, value};
    return i2c_master_transmit(m->dev, frame, sizeof frame, TIMEOUT_MS);
}

esp_err_t mcp23017_init(mcp23017_t *m, i2c_master_bus_handle_t bus, uint8_t address,
                        uint32_t scl_speed_hz)
{
    *m = (mcp23017_t){0};
    i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = address,
        .scl_speed_hz = scl_speed_hz,
    };
    return i2c_master_bus_add_device(bus, &cfg, &m->dev);
}

esp_err_t mcp23017_set_direction(mcp23017_t *m, mcp23017_port_t port, uint8_t iodir)
{
    return write_reg(m, (uint8_t)(REG_IODIR + port), iodir);
}

esp_err_t mcp23017_write_port(mcp23017_t *m, mcp23017_port_t port, uint8_t olat)
{
    /* Remembered before the write: should the write fail, the next one carries the
     * level we meant, not the one before it. */
    m->olat[port] = olat;
    return write_reg(m, (uint8_t)(REG_OLAT + port), olat);
}

esp_err_t mcp23017_write_pin(mcp23017_t *m, mcp23017_port_t port, uint8_t pin, bool level)
{
    uint8_t bit = (uint8_t)(1u << (pin & 7u));
    uint8_t olat = level ? (uint8_t)(m->olat[port] | bit) : (uint8_t)(m->olat[port] & ~bit);
    return mcp23017_write_port(m, port, olat);
}
