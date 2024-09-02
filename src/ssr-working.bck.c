// SPDX-License-Identifier: GPL-2.0+

/*
 * ssr.c - Software RAID
 *
 * Author: Mihnea Tudor mihnea.tudor01@gmail.com
 */
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/list.h>
#include <linux/hashtable.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/uaccess.h>
#include <linux/spinlock.h>
#include <linux/kprobes.h>
#include <linux/miscdevice.h>
#include <linux/types.h>
#include <linux/atomic.h>
#include <linux/ioport.h>
#include <linux/moduleparam.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/interrupt.h>
#include <linux/kfifo.h>
#include <linux/wait.h>
#include <asm/io.h>
#include <linux/genhd.h>
#include <linux/blkdev.h>
#include <linux/crc32.h>

#include "ssr.h"

#define MODULE_NAME "ssr"

struct page_list {
	struct page *page;
	struct list_head list;
};

struct submit_bio_work_item {
    struct work_struct work_str;
	struct bio *bio;
};

static struct raid_blk_dev {
    struct gendisk *gd;
    struct request_queue *queue;
	size_t size;
} raid_dev;

static struct block_device *raid_dev1;
static struct block_device *raid_dev2;

// TODO: For debugging only, remove at the end
int max_cnt = 0;

static void debug_bio_data(struct bio *bio, const char *msg)
{
    struct bio_vec bvec;
    struct bvec_iter it;
    bio_for_each_segment(bvec, bio, it) {
        sector_t sector = it.bi_sector;
        char *page_data = kmap_atomic(bvec.bv_page);
        unsigned long offset = bvec.bv_offset;
        size_t len = bvec.bv_len;
		char bufferino[512];
		memset(bufferino, 0, 512);
		memcpy(bufferino, page_data, 10);

		pr_info("%s\n", msg);
		pr_info("Sector: %lld\n", sector);
		pr_info("Page ptr: 0x%lx\n", (unsigned long)page_data);
		pr_info("Data: %s\n", bufferino);
		pr_info("Offset: %ld\n", offset);
		pr_info("Len: %d\n", len);
		pr_info("\n");

        kunmap_atomic(page_data);
    }
}

static int raid_blk_open(struct block_device *bdev, fmode_t mode)
{
    pr_info("Open test!\n");

	// Get the 2 physical block devices
	raid_dev1 = blkdev_get_by_path(PHYSICAL_DISK1_NAME, FMODE_READ | FMODE_WRITE | FMODE_EXCL, THIS_MODULE);
	if (raid_dev1 == NULL) {
		pr_err("Physical device at path %s not found.\n", PHYSICAL_DISK1_NAME);
		return -ENODEV;
	}

	raid_dev2 = blkdev_get_by_path(PHYSICAL_DISK2_NAME, FMODE_READ | FMODE_WRITE | FMODE_EXCL, THIS_MODULE);
	if (raid_dev2 == NULL) {
		pr_err("Physical device at path %s not found.\n", PHYSICAL_DISK2_NAME);
		blkdev_put(raid_dev1, FMODE_READ | FMODE_WRITE | FMODE_EXCL);
		return -ENODEV;
	}

    return 0;
}

static void raid_blk_release(struct gendisk *gd, fmode_t mode)
{
    pr_info("Release test!\n");

	// Put the 2 physical block devices
	blkdev_put(raid_dev1, FMODE_READ | FMODE_WRITE | FMODE_EXCL);
	blkdev_put(raid_dev2, FMODE_READ | FMODE_WRITE | FMODE_EXCL);
}

static void free_page_list(struct list_head *pages_list)
{
	struct list_head *i, *tmp;
	struct page_list *ple;

	list_for_each_safe(i, tmp, pages_list) {
		ple = list_entry(i, struct page_list, list);

		list_del(i);
		__free_page(ple->page);
		kfree(ple);
	}
}

void send_bio_to_device(struct work_struct *work)
{
	struct submit_bio_work_item *args;
	int ret;

	args = container_of(work, struct submit_bio_work_item,  work_str);

	// Send them to the real block device driver
	ret = submit_bio_wait(args->bio);

	if (ret != 0)
		pr_err("send_bio_to_device Received error code %d\n", ret);
}

static struct bio *create_rw_bio(struct bio *model_bio, struct list_head *pages_list, int device, int is_write)
{
	struct bio *bio;
	struct page *new_page;
    struct bio_vec bvec;
    struct bvec_iter it;
	sector_t sector;
	char *page_data, *new_page_data;
	struct page_list *ple;
	unsigned long offset;
	size_t len;

	// Initialize a new bio
	bio = bio_alloc(GFP_ATOMIC, model_bio->bi_vcnt);
	bio->bi_disk = device == 1 ? raid_dev1->bd_disk : raid_dev2->bd_disk;
	bio->bi_opf = is_write == 1 ? REQ_OP_WRITE : REQ_OP_READ;
	bio->bi_iter.bi_sector = model_bio->bi_iter.bi_sector;

	// For each segment, allocate a new page and add it to the bio
    bio_for_each_segment(bvec, model_bio, it) {
        sector = it.bi_sector;
        page_data = kmap_atomic(bvec.bv_page);
        offset = bvec.bv_offset;
        len = bvec.bv_len;

		// Create a new page
		new_page = alloc_page(GFP_ATOMIC);

		// Add it to the list
		ple = kmalloc(sizeof(*ple), GFP_ATOMIC);
		ple->page = new_page;

		list_add_tail(&ple->list, pages_list);

		// If it's a write operation, memcpy the data over
		if (is_write) {
			new_page_data = kmap_atomic(new_page);
			memcpy(new_page_data + offset, page_data + offset, len);
		}

		// Add it to the bio
		bio_add_page(bio, new_page, len, offset);

        kunmap_atomic(page_data);

		if (is_write)
			kunmap_atomic(new_page_data);
    }

	return bio;
}

static struct bio *create_crc_bio(struct bio *model_bio, struct list_head *pages_list, int device, int is_write)
{
	struct bio *bio;
	struct page *new_page;
    struct bio_vec bvec;
    struct bvec_iter it;
	sector_t start_sector;
	sector_t sector;
	char *page_data, *new_page_data;
	struct page_list *ple;
	unsigned long offset;
	size_t total_len = 0;
	size_t len;
	int curr_segment = -1;
	int last_segment;
	int remaining_size;
	unsigned int crc;
	int i;

	// Compute the starting sector
	// Formula: LOGICAL_DISK_SECTORS + model_start_sector * 4 / 512 (optimized)
	start_sector = LOGICAL_DISK_SECTORS + (model_bio->bi_iter.bi_sector >> 7);

	// Initialize a new bio
	bio = bio_alloc(GFP_ATOMIC, model_bio->bi_vcnt);
	bio->bi_disk = device == 1 ? raid_dev1->bd_disk : raid_dev2->bd_disk;
	bio->bi_opf = is_write == 1 ? REQ_OP_WRITE : REQ_OP_READ;
	bio->bi_iter.bi_sector = start_sector;

	// For each segment, allocate a new page and add it to the bio
    bio_for_each_segment(bvec, model_bio, it) {
        sector = it.bi_sector;

		// Formula: sector * 4 % 4096
        offset = (sector << 2) % 4096;

		// Formula: len (in bytes) / KERNERL_SECTOR_SIZE * 4
        len = bvec.bv_len >> 7;
		total_len += len;

		// Get the segment/page index, after the logical partition,
		// to write in. We need 4 bytes for each sector
		// Formula: sector * 4 / 4096 (optimized)
		curr_segment = sector >> 10;
		if (curr_segment != last_segment) {
			// Update the current segment
			last_segment = curr_segment;

			// Create a new page
			new_page = alloc_page(GFP_ATOMIC);

			// Add it to the list
			ple = kmalloc(sizeof(*ple), GFP_ATOMIC);
			ple->page = new_page;

			list_add_tail(&ple->list, pages_list);
		}

		// If it's a write operation, memcpy the data over
		if (is_write) {
			page_data = kmap_atomic(bvec.bv_page);
			new_page_data = kmap_atomic(new_page);

			// For each sector, calculate the CRC
			for (i = 0; i < (len >> 2); ++i) {
				// Compute the CRC
				// Formula for start pos: page_start + CURR_SECTOR_INDEX * KERNEL_SECTOR_SIZE 
				crc = crc32(0, page_data + i * KERNEL_SECTOR_SIZE, KERNEL_SECTOR_SIZE);

				memcpy(new_page_data + offset + (i << 2), &crc, 4);
			}
		}

		// Add the segment to the bio, or merge it if it's the same page
		bio_add_page(bio, new_page, len, offset);

		if (is_write) {
			kunmap_atomic(page_data);
			kunmap_atomic(new_page_data);
		}
    }

	// Make the bio size a multiple of KERNEL_SECTOR_SIZE by creating a new page
	remaining_size = KERNEL_SECTOR_SIZE - (total_len % KERNEL_SECTOR_SIZE);
	if (remaining_size < KERNEL_SECTOR_SIZE) {
		pr_info("Remaining size: %d\n", remaining_size);

		// new_page = alloc_page(GFP_ATOMIC);

		// Add it to the list
		// ple = kmalloc(sizeof(*ple), GFP_ATOMIC);
		// ple->page = new_page;

		// list_add_tail(&ple->list, pages_list);
		
		int ret = bio_add_page(bio, new_page, remaining_size, offset + len);
		pr_info("Add page bytes: %d\n", ret);
	}
	pr_info("Total len: %d\n", total_len);

	return bio;
}

static blk_qc_t raid_blk_submit_bio(struct bio *received_bio)
{
	struct bio *bio_rw_disk1, *bio_rw_disk2;
	struct bio *bio_crc_disk1, *bio_crc_disk2;
	struct submit_bio_work_item submit_bio1_rw;
	struct submit_bio_work_item submit_bio2_rw;
	struct submit_bio_work_item submit_bio1_crc;
	struct submit_bio_work_item submit_bio2_crc;
	LIST_HEAD(pages_rw_disk1);
	LIST_HEAD(pages_rw_disk2);
	LIST_HEAD(pages_crc_disk1);
	LIST_HEAD(pages_crc_disk2);
    struct bio_vec bvec;
    struct bvec_iter it;
	int is_write;
	struct page_list *curr_ple;
	char *curr_page, *recv_page_data;
	int offset;
	int len;

	is_write = bio_data_dir(received_bio);

	// pr_info("submit_bio Direction %s\n", is_write == 0 ? "read" : "write");

	// Create the bios to be sent down
	bio_rw_disk1 = create_rw_bio(received_bio, &pages_rw_disk1, 1, is_write);
	bio_rw_disk2 = create_rw_bio(received_bio, &pages_rw_disk2, 2, is_write);
	bio_crc_disk1 = create_crc_bio(received_bio, &pages_crc_disk1, 1, is_write);
	bio_crc_disk2 = create_crc_bio(received_bio, &pages_crc_disk2, 2, is_write);

	// pr_info("Segments remaining in disk1 bio BEFORE: %d\n", bio_rw_disk1->bi_vcnt);
	// pr_info("Size of disk1 BEFORE: %d\n", bio_rw_disk1->bi_iter.bi_size);

	// debug_bio_data(bio_rw_disk1, "Disk1 data before:");
	// debug_bio_data(bio_rw_disk2, "Disk2 data before:");
	debug_bio_data(bio_crc_disk1, "CRC1 data before:");
	
	// Schedule work to send each bio to the appropriate physical disk
	INIT_WORK(&submit_bio1_rw.work_str, send_bio_to_device);
	INIT_WORK(&submit_bio2_rw.work_str, send_bio_to_device);
	INIT_WORK(&submit_bio1_crc.work_str, send_bio_to_device);
	INIT_WORK(&submit_bio2_crc.work_str, send_bio_to_device);

	// Configure the arguments
	submit_bio1_rw.bio = bio_rw_disk1;
	submit_bio2_rw.bio = bio_rw_disk2;
	submit_bio1_crc.bio = bio_crc_disk1;
	submit_bio2_crc.bio = bio_crc_disk2;

	// Schedule the work
	schedule_work(&submit_bio1_rw.work_str);
	schedule_work(&submit_bio2_rw.work_str);
	schedule_work(&submit_bio1_crc.work_str);
	schedule_work(&submit_bio2_crc.work_str);

	// Wait for the bios to return
	flush_scheduled_work();

	// Obtain the virtual address from the page (either extract directly from LOW_MEM or search with HIGH_MEM)
	// kmap_atomic(page_from_down)
	
	// raid logic.....?!

	// memcpy() with virtual addressed of the DMAd page and the recv bio page??
	// Translation:
	// Memcpy the pages, received from the block devices, back into the received bio
	// Use kmap_atomic and kunmap_atomic for each
	// Probably do a per-segment approach

	// TEMPORARY: Write back what's received from disk 1
	// Does this work??? Otherwise we'd have to iterate through both bios at once..
	// Seems like it works :)
	// Or maybe not????????!!!!!!
	// bio_copy_data(received_bio, bio_rw_disk1);

	// Copy the pages manually back to the received bio
	curr_ple = list_first_entry(&pages_rw_disk1, struct page_list, list);
    bio_for_each_segment(bvec, received_bio, it) {
		curr_page = kmap_atomic(curr_ple->page);
        recv_page_data = kmap_atomic(bvec.bv_page);
        offset = bvec.bv_offset;
        len = bvec.bv_len;

		memcpy(recv_page_data + offset, curr_page + offset, len);

        kunmap_atomic(recv_page_data);
        kunmap_atomic(curr_page);

		curr_ple = list_next_entry(curr_ple, list);
    }

	// pr_info("Segments remaining in disk1 bio AFTER: %d\n", bio_rw_disk1->bi_vcnt);
	// pr_info("Size of disk1 AFTER: %d\n", bio_rw_disk1->bi_iter.bi_size);

	
	// TODO: Remove this
	// debug_bio_data(received_bio, "Received data at the END:");



	// pr_info("Segments remaining in disk1 bio AFTER 2: %d\n", bio_rw_disk1->bi_vcnt);

	// Free up the memory
	free_page_list(&pages_rw_disk1);
	free_page_list(&pages_rw_disk2);
	free_page_list(&pages_crc_disk1);
	free_page_list(&pages_crc_disk2);

	bio_put(bio_rw_disk1);
	bio_put(bio_rw_disk2);
	bio_put(bio_crc_disk1);
	bio_put(bio_crc_disk2);

	// pr_info("Segments remaining in disk1 bio AFTER 3: %d\n", bio_rw_disk1->bi_vcnt);


	bio_endio(received_bio);
	return BLK_QC_T_NONE;
}

const struct block_device_operations raid_blk_ops = {
    .owner = THIS_MODULE,
    .open = raid_blk_open,
    .release = raid_blk_release,
	.submit_bio = raid_blk_submit_bio
};

static int create_blk_dev(struct raid_blk_dev *blk_dev)
{
	// Set the size
	blk_dev->size = LOGICAL_DISK_SIZE;

	// ALlocate the queue
	blk_dev->queue = blk_alloc_queue(NUMA_NO_NODE);
	if (blk_dev->queue == NULL) {
		pr_err("failed request queue allocation\n");
		return -ENOMEM;
	}

    blk_queue_logical_block_size(blk_dev->queue, KERNEL_SECTOR_SIZE);
    blk_dev->queue->queuedata = blk_dev;

    blk_dev->gd = alloc_disk(SSR_NUM_MINORS);
    if (!blk_dev->gd) {
        pr_err("alloc_disk failure\n");
        return -ENOMEM;
    }

	// Set gendisk properties
	memcpy(blk_dev->gd->disk_name, MODULE_NAME, 4);
	set_capacity(blk_dev->gd, LOGICAL_DISK_SECTORS);
	
	blk_dev->gd->major = SSR_MAJOR;
	blk_dev->gd->first_minor = SSR_FIRST_MINOR;
	blk_dev->gd->fops = &raid_blk_ops;
	blk_dev->gd->queue = blk_dev->queue;
	blk_dev->gd->private_data = blk_dev;

    add_disk(blk_dev->gd);

	return 0;
}

static void delete_blk_dev(struct raid_blk_dev *dev)
{
    if (dev->gd) {
        del_gendisk(dev->gd);
		put_disk(dev->gd);
	}

    blk_cleanup_queue(dev->queue);
}

static int raid_init(void)
{
	int ret;

	ret = register_blkdev(SSR_MAJOR, MODULE_NAME);
	if (ret < 0) {
		pr_err("unable to register ssr block device\n");
		return -EBUSY;
	}

    ret = create_blk_dev(&raid_dev);
	if (ret < 0) {
		unregister_blkdev(SSR_MAJOR, MODULE_NAME);
		return ret;
	}

	return 0;
}

static void raid_exit(void)
{
    delete_blk_dev(&raid_dev);
	unregister_blkdev(SSR_MAJOR, MODULE_NAME);
}

module_init(raid_init);
module_exit(raid_exit);

MODULE_DESCRIPTION("Software RAID");
MODULE_AUTHOR("Mihnea Tudor mihnea.tudor01@gmail.com");
MODULE_LICENSE("GPL v2");
