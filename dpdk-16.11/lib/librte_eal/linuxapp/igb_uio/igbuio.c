/*-
 * GPL LICENSE SUMMARY
 *
 *   Copyright(c) 2010-2014 Intel Corporation. All rights reserved.
 *
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of version 2 of the GNU General Public License as
 *   published by the Free Software Foundation.
 *
 *   This program is distributed in the hope that it will be useful, but
 *   WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *   General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program; if not, write to the Free Software
 *   Foundation, Inc., 51 Franklin St - Fifth Floor, Boston, MA 02110-1301 USA.
 *   The full GNU General Public License is included in this distribution
 *   in the file called LICENSE.GPL.
 *
 *   Contact Information:
 *   Intel Corporation
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/device.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/uio_driver.h>
#include <linux/io.h>
#include <linux/msi.h>
#include <linux/version.h>
#include <linux/slab.h>
#include "igb_uio.h"

#ifdef CONFIG_XEN_DOM0
#include <xen/xen.h>
#endif
#include <rte_pci_dev_features.h>

#include "compat.h"

/**
 * A structure describing the private information for a uio device.
 */
struct rte_uio_pci_dev {
	struct uio_info info;
	struct pci_dev *pdev;
	enum rte_intr_mode mode;
	struct net_adapter *adapter;
};

static char *intr_mode;
static enum rte_intr_mode igbuio_intr_mode_preferred = RTE_INTR_MODE_MSIX;
struct stats_struct sarrays[MAX_DEVICES][MAX_QID] = {{{0, 0, 0, 0, 0, 0}}};
struct stats_struct old_sarrays[MAX_DEVICES][MAX_QID] = {{{0, 0, 0, 0, 0, 0}}};

/* sriov sysfs */
static ssize_t
show_max_vfs(struct device *dev, struct device_attribute *attr,
	     char *buf)
{
	return snprintf(buf, 10, "%u\n", dev_num_vf(dev));
}

static ssize_t
store_max_vfs(struct device *dev, struct device_attribute *attr,
	      const char *buf, size_t count)
{
	int err = 0;
	unsigned long max_vfs;
	struct pci_dev *pdev = to_pci_dev(dev);

	if (0 != kstrtoul(buf, 0, &max_vfs))
		return -EINVAL;

	if (0 == max_vfs)
		pci_disable_sriov(pdev);
	else if (0 == pci_num_vf(pdev))
		err = pci_enable_sriov(pdev, max_vfs);
	else /* do nothing if change max_vfs number */
		err = -EINVAL;

	return err ? err : count;
}

static DEVICE_ATTR(max_vfs, S_IRUGO | S_IWUSR, show_max_vfs, store_max_vfs);

static struct attribute *dev_attrs[] = {
	&dev_attr_max_vfs.attr,
	NULL,
};

static const struct attribute_group dev_attr_grp = {
	.attrs = dev_attrs,
};
/*
 * It masks the msix on/off of generating MSI-X messages.
 */
static void
igbuio_msix_mask_irq(struct msi_desc *desc, int32_t state)
{
	u32 mask_bits = desc->masked;
	unsigned offset = desc->msi_attrib.entry_nr * PCI_MSIX_ENTRY_SIZE +
						PCI_MSIX_ENTRY_VECTOR_CTRL;

	if (state != 0)
		mask_bits &= ~PCI_MSIX_ENTRY_CTRL_MASKBIT;
	else
		mask_bits |= PCI_MSIX_ENTRY_CTRL_MASKBIT;

	if (mask_bits != desc->masked) {
		writel(mask_bits, desc->mask_base + offset);
		readl(desc->mask_base);
		desc->masked = mask_bits;
	}
}

/**
 * This is the irqcontrol callback to be registered to uio_info.
 * It can be used to disable/enable interrupt from user space processes.
 *
 * @param info
 *  pointer to uio_info.
 * @param irq_state
 *  state value. 1 to enable interrupt, 0 to disable interrupt.
 *
 * @return
 *  - On success, 0.
 *  - On failure, a negative value.
 */
static int
igbuio_pci_irqcontrol(struct uio_info *info, s32 irq_state)
{
	struct rte_uio_pci_dev *udev = info->priv;
	struct pci_dev *pdev = udev->pdev;

	pci_cfg_access_lock(pdev);
	if (udev->mode == RTE_INTR_MODE_LEGACY)
		pci_intx(pdev, !!irq_state);

	else if (udev->mode == RTE_INTR_MODE_MSIX) {
		struct msi_desc *desc;

#if (LINUX_VERSION_CODE < KERNEL_VERSION(4, 3, 0))
		list_for_each_entry(desc, &pdev->msi_list, list)
			igbuio_msix_mask_irq(desc, irq_state);
#else
		list_for_each_entry(desc, &pdev->dev.msi_list, list)
			igbuio_msix_mask_irq(desc, irq_state);
#endif
	}
	pci_cfg_access_unlock(pdev);

	return 0;
}

/**
 * This is interrupt handler which will check if the interrupt is for the right device.
 * If yes, disable it here and will be enable later.
 */
static irqreturn_t
igbuio_pci_irqhandler(int irq, struct uio_info *info)
{
	struct rte_uio_pci_dev *udev = info->priv;

	/* Legacy mode need to mask in hardware */
	if (udev->mode == RTE_INTR_MODE_LEGACY &&
	    !pci_check_and_mask_intx(udev->pdev))
		return IRQ_NONE;

	/* Message signal mode, no share IRQ and automasked */
	return IRQ_HANDLED;
}

#ifdef CONFIG_XEN_DOM0
static int
igbuio_dom0_mmap_phys(struct uio_info *info, struct vm_area_struct *vma)
{
	int idx;

	idx = (int)vma->vm_pgoff;
	vma->vm_page_prot = pgprot_noncached(vma->vm_page_prot);
#ifdef HAVE_PTE_MASK_PAGE_IOMAP
#if LINUX_VERSION_CODE < KERNEL_VERSION(3, 18, 0)
	vma->vm_page_prot.pgprot |= _PAGE_IOMAP;
#else
	vma->vm_page_prot.pgprot |= _PAGE_SOFTW2;
#endif
#endif

	return remap_pfn_range(vma,
			vma->vm_start,
			info->mem[idx].addr >> PAGE_SHIFT,
			vma->vm_end - vma->vm_start,
			vma->vm_page_prot);
}

/**
 * This is uio device mmap method which will use igbuio mmap for Xen
 * Dom0 environment.
 */
static int
igbuio_dom0_pci_mmap(struct uio_info *info, struct vm_area_struct *vma)
{
	int idx;

	if (vma->vm_pgoff >= MAX_UIO_MAPS)
		return -EINVAL;

	if (info->mem[vma->vm_pgoff].size == 0)
		return -EINVAL;

	idx = (int)vma->vm_pgoff;
	switch (info->mem[idx].memtype) {
	case UIO_MEM_PHYS:
		return igbuio_dom0_mmap_phys(info, vma);
	case UIO_MEM_LOGICAL:
	case UIO_MEM_VIRTUAL:
	default:
		return -EINVAL;
	}
}
#endif

/* Remap pci resources described by bar #pci_bar in uio resource n. */
static int
igbuio_pci_setup_iomem(struct pci_dev *dev, struct uio_info *info,
		       int n, int pci_bar, const char *name)
{
	unsigned long addr, len;
	void *internal_addr;

	if (n >= ARRAY_SIZE(info->mem))
		return -EINVAL;

	addr = pci_resource_start(dev, pci_bar);
	len = pci_resource_len(dev, pci_bar);
	if (addr == 0 || len == 0)
		return -1;
	internal_addr = ioremap(addr, len);
	if (internal_addr == NULL)
		return -1;
	info->mem[n].name = name;
	info->mem[n].addr = addr;
	info->mem[n].internal_addr = internal_addr;
	info->mem[n].size = len;
	info->mem[n].memtype = UIO_MEM_PHYS;
	return 0;
}

/* Get pci port io resources described by bar #pci_bar in uio resource n. */
static int
igbuio_pci_setup_ioport(struct pci_dev *dev, struct uio_info *info,
		int n, int pci_bar, const char *name)
{
	unsigned long addr, len;

	if (n >= ARRAY_SIZE(info->port))
		return -EINVAL;

	addr = pci_resource_start(dev, pci_bar);
	len = pci_resource_len(dev, pci_bar);
	if (addr == 0 || len == 0)
		return -EINVAL;

	info->port[n].name = name;
	info->port[n].start = addr;
	info->port[n].size = len;
	info->port[n].porttype = UIO_PORT_X86;

	return 0;
}

/* Unmap previously ioremap'd resources */
static void
igbuio_pci_release_iomem(struct uio_info *info)
{
	int i;

	for (i = 0; i < MAX_UIO_MAPS; i++) {
		if (info->mem[i].internal_addr)
			iounmap(info->mem[i].internal_addr);
	}
}

static int
igbuio_setup_bars(struct pci_dev *dev, struct uio_info *info)
{
	int i, iom, iop, ret;
	unsigned long flags;
	static const char *bar_names[PCI_STD_RESOURCE_END + 1]  = {
		"BAR0",
		"BAR1",
		"BAR2",
		"BAR3",
		"BAR4",
		"BAR5",
	};

	iom = 0;
	iop = 0;

	for (i = 0; i < ARRAY_SIZE(bar_names); i++) {
		if (pci_resource_len(dev, i) != 0 &&
				pci_resource_start(dev, i) != 0) {
			flags = pci_resource_flags(dev, i);
			if (flags & IORESOURCE_MEM) {
				ret = igbuio_pci_setup_iomem(dev, info, iom,
							     i, bar_names[i]);
				if (ret != 0)
					return ret;
				iom++;
			} else if (flags & IORESOURCE_IO) {
				ret = igbuio_pci_setup_ioport(dev, info, iop,
							      i, bar_names[i]);
				if (ret != 0)
					return ret;
				iop++;
			}
		}
	}

	return (iom != 0) ? ret : -ENOENT;
}

#if LINUX_VERSION_CODE < KERNEL_VERSION(3, 8, 0)
static int __devinit
#else
static int
#endif
igbuio_pci_probe(struct pci_dev *dev, const struct pci_device_id *id)
{
	struct rte_uio_pci_dev *udev;
	struct msix_entry msix_entry;
	int err;

	/* essential vars for configuring the device with net_device */
	struct net_device *netdev;
	struct net_adapter *adapter = NULL;
	struct ixgbe_hw *hw_i = NULL;
	struct e1000_hw *hw_e = NULL;
	struct i40e_hw *hw_i4 = NULL;	

	udev = kzalloc(sizeof(struct rte_uio_pci_dev), GFP_KERNEL);
	if (!udev)
		return -ENOMEM;

	/*
	 * enable device: ask low-level code to enable I/O and
	 * memory
	 */
	err = pci_enable_device(dev);
	if (err != 0) {
		dev_err(&dev->dev, "Cannot enable PCI device\n");
		goto fail_free;
	}

	/* enable bus mastering on the device */
	pci_set_master(dev);

	/* remap IO memory */
	err = igbuio_setup_bars(dev, &udev->info);
	if (err != 0)
		goto fail_release_iomem;

	/* set 64-bit DMA mask */
	err = pci_set_dma_mask(dev,  DMA_BIT_MASK(64));
	if (err != 0) {
		dev_err(&dev->dev, "Cannot set DMA mask\n");
		goto fail_release_iomem;
	}

	err = pci_set_consistent_dma_mask(dev, DMA_BIT_MASK(64));
	if (err != 0) {
		dev_err(&dev->dev, "Cannot set consistent DMA mask\n");
		goto fail_release_iomem;
	}

	/* fill uio infos */
	udev->info.name = "igb_uio";
	udev->info.version = "0.1";
	udev->info.handler = igbuio_pci_irqhandler;
	udev->info.irqcontrol = igbuio_pci_irqcontrol;
#ifdef CONFIG_XEN_DOM0
	/* check if the driver run on Xen Dom0 */
	if (xen_initial_domain())
		udev->info.mmap = igbuio_dom0_pci_mmap;
#endif
	udev->info.priv = udev;
	udev->pdev = dev;

	switch (igbuio_intr_mode_preferred) {
	case RTE_INTR_MODE_MSIX:
		/* Only 1 msi-x vector needed */
		msix_entry.entry = 0;
		if (pci_enable_msix(dev, &msix_entry, 1) == 0) {
			dev_dbg(&dev->dev, "using MSI-X");
			udev->info.irq = msix_entry.vector;
			udev->mode = RTE_INTR_MODE_MSIX;
			break;
		}
		/* fall back to INTX */
	case RTE_INTR_MODE_LEGACY:
		if (pci_intx_mask_supported(dev)) {
			dev_dbg(&dev->dev, "using INTX");
			udev->info.irq_flags = IRQF_SHARED;
			udev->info.irq = dev->irq;
			udev->mode = RTE_INTR_MODE_LEGACY;
			break;
		}
		dev_notice(&dev->dev, "PCI INTX mask not supported\n");
		/* fall back to no IRQ */
	case RTE_INTR_MODE_NONE:
		udev->mode = RTE_INTR_MODE_NONE;
		udev->info.irq = 0;
		break;

	default:
		dev_err(&dev->dev, "invalid IRQ mode %u",
			igbuio_intr_mode_preferred);
		err = -EINVAL;
		goto fail_release_iomem;
	}

	err = sysfs_create_group(&dev->dev.kobj, &dev_attr_grp);
	if (err != 0)
		goto fail_release_iomem;

	/* initialize the corresponding netdev */
        netdev = alloc_etherdev(sizeof(struct net_adapter));
        if (!netdev) {
                err = -ENOMEM;
                goto fail_alloc_etherdev;
        }
        SET_NETDEV_DEV(netdev, pci_dev_to_dev(dev));
        adapter = netdev_priv(netdev);
        adapter->netdev = netdev;
        adapter->pdev = dev;
        udev->adapter = adapter;
        adapter->type = retrieve_dev_specs(id);
	/* recover device-specific mac address */
        switch (adapter->type) {
        case IXGBE:
                hw_i = &adapter->hw._ixgbe_hw;
                hw_i->back = adapter;
                hw_i->hw_addr = ioremap(pci_resource_start(dev, 0),
					pci_resource_len(dev, 0));
                if (!hw_i->hw_addr) {
                        err = -EIO;
                        goto fail_ioremap;
                }
                break;
        case IGB:
                hw_e = &adapter->hw._e1000_hw;
                hw_e->back = adapter;
                hw_e->hw_addr = ioremap(pci_resource_start(dev, 0),
					pci_resource_len(dev, 0));
                if (!hw_e->hw_addr) {
                        err = -EIO;
                        goto fail_ioremap;
                }
                break;
	case I40E:
		hw_i4 = &adapter->hw._i40e_hw;
		hw_i4->back = adapter;

		if (i40e_get_local_mac_addr(dev, id, hw_i4->mac.addr)) {
			err = -EIO;
			goto fail_ioremap;
		}
		break;
        }
	
        netdev_assign_netdev_ops(netdev);
        strncpy(netdev->name, pci_name(dev), sizeof(netdev->name) - 1);
        retrieve_dev_addr(netdev, adapter);
        strcpy(netdev->name, "dpdk%d");
        err = register_netdev(netdev);
        if (err)
                goto fail_ioremap;
        adapter->netdev_registered = true;

	if (sscanf(netdev->name, "dpdk%hu", &adapter->bd_number) <= 0)
                goto fail_bdnumber;

        //printk(KERN_DEBUG "ifindex picked: %hu\n", adapter->bd_number);                             
        dev_info(&dev->dev, "ifindex picked: %hu\n", adapter->bd_number);

	/* register uio driver */
	err = uio_register_device(&dev->dev, &udev->info);
	if (err != 0)
		goto fail_remove_group;

	pci_set_drvdata(dev, udev);

	dev_info(&dev->dev, "uio device registered with irq %lx\n",
		 udev->info.irq);

        /* reset nstats */
	memset(&adapter->nstats, 0, sizeof(struct net_device_stats));

	return 0;

 fail_bdnumber:
 fail_ioremap:
	free_netdev(netdev);
 fail_alloc_etherdev:
	pci_release_selected_regions(dev,
                                     pci_select_bars(dev, IORESOURCE_MEM));
 fail_remove_group:
	sysfs_remove_group(&dev->dev.kobj, &dev_attr_grp);
 fail_release_iomem:
	igbuio_pci_release_iomem(&udev->info);
	if (udev->mode == RTE_INTR_MODE_MSIX)
		pci_disable_msix(udev->pdev);
	pci_disable_device(dev);
 fail_free:
	kfree(udev);

	return err;
}

static void
igbuio_pci_remove(struct pci_dev *dev)
{
	struct rte_uio_pci_dev *udev = pci_get_drvdata(dev);
	struct net_device *netdev;
	struct net_adapter *adapter;

	/* unregister device from netdev */
	netdev = udev->adapter->netdev;
	adapter = netdev_priv(netdev);

	if (udev->adapter->netdev_registered) {
		unregister_netdev(netdev);
		udev->adapter->netdev_registered = false;
	}
	switch (udev->adapter->type) {
	case IXGBE:
		iounmap(udev->adapter->hw._ixgbe_hw.hw_addr);
		break;
	case IGB:
		iounmap(udev->adapter->hw._e1000_hw.hw_addr);
		break;
	case I40E:
		/* do nothing */
		break;
	}
	free_netdev(netdev);	

	sysfs_remove_group(&dev->dev.kobj, &dev_attr_grp);
	uio_unregister_device(&udev->info);
	igbuio_pci_release_iomem(&udev->info);
	if (udev->mode == RTE_INTR_MODE_MSIX)
		pci_disable_msix(dev);
	pci_disable_device(dev);
	pci_set_drvdata(dev, NULL);
	kfree(udev);
}

static int
igbuio_config_intr_mode(char *intr_str)
{
	if (!intr_str) {
		pr_info("Use MSIX interrupt by default\n");
		return 0;
	}

	if (!strcmp(intr_str, RTE_INTR_MODE_MSIX_NAME)) {
		igbuio_intr_mode_preferred = RTE_INTR_MODE_MSIX;
		pr_info("Use MSIX interrupt\n");
	} else if (!strcmp(intr_str, RTE_INTR_MODE_LEGACY_NAME)) {
		igbuio_intr_mode_preferred = RTE_INTR_MODE_LEGACY;
		pr_info("Use legacy interrupt\n");
	} else {
		pr_info("Error: bad parameter - %s\n", intr_str);
		return -EINVAL;
	}

	return 0;
}

static int
update_stats(struct stats_struct *stats)
{
	uint8_t qid = stats->qid;
	uint8_t device = stats->dev;

	if (unlikely(sarrays[device][qid].rx_bytes > stats->rx_bytes ||
		     sarrays[device][qid].tx_bytes > stats->tx_bytes)) {
		/* mTCP app restarted?? */
		old_sarrays[device][qid].rx_bytes += sarrays[device][qid].rx_bytes;
		old_sarrays[device][qid].rx_pkts += sarrays[device][qid].rx_pkts;
		old_sarrays[device][qid].tx_bytes += sarrays[device][qid].tx_bytes;
		old_sarrays[device][qid].tx_pkts += sarrays[device][qid].tx_pkts;
	}

	sarrays[device][qid].rx_bytes = stats->rx_bytes;
	sarrays[device][qid].rx_pkts = stats->rx_pkts;
	sarrays[device][qid].tx_bytes = stats->tx_bytes;
	sarrays[device][qid].tx_pkts = stats->tx_pkts;

#if 0
	printk(KERN_ALERT "Dev: %d, Qid: %d, RXP: %llu, RXB: %llu, TXP: %llu, TXB: %llu\n",
	       device, qid,
	       (long long unsigned int)sarrays[device][qid].rx_pkts,
	       (long long unsigned int)sarrays[device][qid].rx_bytes,
	       (long long unsigned int)sarrays[device][qid].tx_pkts,
	       (long long unsigned int)sarrays[device][qid].tx_bytes);
#endif
	return 0;
}

int
igb_net_open(struct inode *inode, struct file *filp)
{
	return 0;
}

int
igb_net_release(struct inode *inode, struct file *filp)
{
	return 0;
}

long
igb_net_ioctl(struct file *filp,
	 unsigned int cmd, unsigned long arg)
{
	int ret = 0;
	struct stats_struct ss;

	copy_from_user(&ss,
		       (struct stats_struct __user *)arg,
		       sizeof(struct stats_struct));

	switch (cmd) {
	case SEND_STATS:
		ret = update_stats(&ss);
		break;
	default:
		ret = -ENOTTY;
	}
	return ret;
}

static struct file_operations igb_net_fops = {
	.open = igb_net_open,
	.release = igb_net_release,
	.unlocked_ioctl = igb_net_ioctl,
};

static struct pci_driver igbuio_pci_driver = {
	.name = "igb_uio",
	.id_table = NULL,
	.probe = igbuio_pci_probe,
	.remove = igbuio_pci_remove,
};

static int __init
igbuio_pci_init_module(void)
{
	int ret;

	ret = igbuio_config_intr_mode(intr_mode);
	if (ret < 0)
		return ret;

	ret = register_chrdev(MAJOR_NO/* MAJOR */,
			      DEV_NAME /*NAME*/,
			      &igb_net_fops);
	if (ret < 0) {
		printk(KERN_ERR "register_chrdev failed\n");
		return ret;
	}
	
	return pci_register_driver(&igbuio_pci_driver);
}

static void __exit
igbuio_pci_exit_module(void)
{
	unregister_chrdev(MAJOR_NO, DEV_NAME);
	pci_unregister_driver(&igbuio_pci_driver);
}

module_init(igbuio_pci_init_module);
module_exit(igbuio_pci_exit_module);

module_param(intr_mode, charp, S_IRUGO);
MODULE_PARM_DESC(intr_mode,
"igb_uio interrupt mode (default=msix):\n"
"    " RTE_INTR_MODE_MSIX_NAME "       Use MSIX interrupt\n"
"    " RTE_INTR_MODE_LEGACY_NAME "     Use Legacy interrupt\n"
"\n");

MODULE_DESCRIPTION("UIO driver for Intel IGB PCI cards");
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Intel Corporation");
