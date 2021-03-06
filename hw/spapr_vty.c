#include "qdev.h"
#include "qemu-char.h"
#include "hw/spapr.h"
#include "hw/spapr_vio.h"

#define VTERM_BUFSIZE   16

typedef struct VIOsPAPRVTYDevice {
    VIOsPAPRDevice sdev;
    CharDriverState *chardev;
    uint32_t in, out;
    uint8_t buf[VTERM_BUFSIZE];
} VIOsPAPRVTYDevice;

static int vty_can_receive(void *opaque)
{
    VIOsPAPRVTYDevice *dev = (VIOsPAPRVTYDevice *)opaque;

    return (dev->in - dev->out) < VTERM_BUFSIZE;
}

static void vty_receive(void *opaque, const uint8_t *buf, int size)
{
    VIOsPAPRVTYDevice *dev = (VIOsPAPRVTYDevice *)opaque;
    int i;

    if ((dev->in == dev->out) && size) {
        /* toggle line to simulate edge interrupt */
        qemu_irq_pulse(dev->sdev.qirq);
    }
    for (i = 0; i < size; i++) {
        assert((dev->in - dev->out) < VTERM_BUFSIZE);
        dev->buf[dev->in++ % VTERM_BUFSIZE] = buf[i];
    }
}

static int vty_getchars(VIOsPAPRDevice *sdev, uint8_t *buf, int max)
{
    VIOsPAPRVTYDevice *dev = (VIOsPAPRVTYDevice *)sdev;
    int n = 0;

    while ((n < max) && (dev->out != dev->in)) {
        buf[n++] = dev->buf[dev->out++ % VTERM_BUFSIZE];
    }

    return n;
}

void vty_putchars(VIOsPAPRDevice *sdev, uint8_t *buf, int len)
{
    VIOsPAPRVTYDevice *dev = (VIOsPAPRVTYDevice *)sdev;

    /* FIXME: should check the qemu_chr_fe_write() return value */
    qemu_chr_fe_write(dev->chardev, buf, len);
}

static int spapr_vty_init(VIOsPAPRDevice *sdev)
{
    VIOsPAPRVTYDevice *dev = (VIOsPAPRVTYDevice *)sdev;

    qemu_chr_add_handlers(dev->chardev, vty_can_receive,
                          vty_receive, NULL, dev);

    return 0;
}

static target_ulong h_put_term_char(CPUState *env, sPAPREnvironment *spapr,
                                    target_ulong opcode, target_ulong *args)
{
    target_ulong reg = args[0];
    target_ulong len = args[1];
    target_ulong char0_7 = args[2];
    target_ulong char8_15 = args[3];
    VIOsPAPRDevice *sdev = spapr_vio_find_by_reg(spapr->vio_bus, reg);
    uint8_t buf[16];

    if (!sdev) {
        return H_PARAMETER;
    }

    if (len > 16) {
        return H_PARAMETER;
    }

    *((uint64_t *)buf) = cpu_to_be64(char0_7);
    *((uint64_t *)buf + 1) = cpu_to_be64(char8_15);

    vty_putchars(sdev, buf, len);

    return H_SUCCESS;
}

static target_ulong h_get_term_char(CPUState *env, sPAPREnvironment *spapr,
                                    target_ulong opcode, target_ulong *args)
{
    target_ulong reg = args[0];
    target_ulong *len = args + 0;
    target_ulong *char0_7 = args + 1;
    target_ulong *char8_15 = args + 2;
    VIOsPAPRDevice *sdev = spapr_vio_find_by_reg(spapr->vio_bus, reg);
    uint8_t buf[16];

    if (!sdev) {
        return H_PARAMETER;
    }

    *len = vty_getchars(sdev, buf, sizeof(buf));
    if (*len < 16) {
        memset(buf + *len, 0, 16 - *len);
    }

    *char0_7 = be64_to_cpu(*((uint64_t *)buf));
    *char8_15 = be64_to_cpu(*((uint64_t *)buf + 1));

    return H_SUCCESS;
}

void spapr_vty_create(VIOsPAPRBus *bus,
                      uint32_t reg, CharDriverState *chardev,
                      qemu_irq qirq, uint32_t vio_irq_num)
{
    DeviceState *dev;
    VIOsPAPRDevice *sdev;

    dev = qdev_create(&bus->bus, "spapr-vty");
    qdev_prop_set_uint32(dev, "reg", reg);
    qdev_prop_set_chr(dev, "chardev", chardev);
    qdev_init_nofail(dev);
    sdev = (VIOsPAPRDevice *)dev;
    sdev->qirq = qirq;
    sdev->vio_irq_num = vio_irq_num;
}

static void vty_hcalls(VIOsPAPRBus *bus)
{
    spapr_register_hypercall(H_PUT_TERM_CHAR, h_put_term_char);
    spapr_register_hypercall(H_GET_TERM_CHAR, h_get_term_char);
}

static VIOsPAPRDeviceInfo spapr_vty = {
    .init = spapr_vty_init,
    .dt_name = "vty",
    .dt_type = "serial",
    .dt_compatible = "hvterm1",
    .hcalls = vty_hcalls,
    .qdev.name = "spapr-vty",
    .qdev.size = sizeof(VIOsPAPRVTYDevice),
    .qdev.props = (Property[]) {
        DEFINE_PROP_UINT32("reg", VIOsPAPRDevice, reg, 0),
        DEFINE_PROP_CHR("chardev", VIOsPAPRVTYDevice, chardev),
        DEFINE_PROP_END_OF_LIST(),
    },
};

static void spapr_vty_register(void)
{
    spapr_vio_bus_register_withprop(&spapr_vty);
}
device_init(spapr_vty_register);
