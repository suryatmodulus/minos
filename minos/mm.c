/*
 * Copyright (C) 2018 Min Le (lemin9538@gmail.com)
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <minos/types.h>
#include <config/config.h>
#include <minos/spinlock.h>
#include <minos/minos.h>
#include <minos/init.h>
#include <minos/mmu.h>
#include <minos/mm.h>
#include <minos/virt.h>

extern unsigned char __code_start;
extern unsigned char __code_end;

struct mem_section {
	unsigned long phy_base;
	int id;
	size_t size;
	size_t nr_blocks;
	size_t free_blocks;
	unsigned long *bitmap;
	unsigned long bm_end;
	unsigned long bm_current;
	spinlock_t lock;
	struct mem_block *blocks;
};

struct slab_header;
struct slab_header {
	unsigned long size;
	union {
		unsigned long magic;
		struct slab_header *next;
	};
};

struct slab_pool {
	size_t size;
	struct slab_header *head;
	unsigned long nr;
};

struct slab {
	int pool_nr;
	int free_size;
	unsigned long slab_free;
	struct list_head block_list;
	struct slab_pool *pool;
	spinlock_t lock;
	size_t alloc_blocks;
};

struct page_pool {
	spinlock_t lock;
	uint32_t meta_blocks;
	uint32_t page_blocks;
	struct list_head meta_list;
	struct list_head block_list;
};

#define PAGE_METAS_IN_BLOCK	(254)
#define BLOCK_BITMAP_SIZE	(64)
#define PAGE_METAS_BITMAP_SIZE	(DIV_ROUND_UP(PAGE_METAS_IN_BLOCK, BITS_PER_BYTE))
#define PAGE_META_SIZE		(BLOCK_BITMAP_SIZE + PAGES_IN_BLOCK * sizeof(struct page))

static int nr_sections;
struct mem_section mem_sections[MAX_MEM_SECTIONS];
static struct slab slab;
static struct slab *pslab = &slab;
static struct page_pool __page_pool;
static struct page_pool *page_pool = &__page_pool;
static size_t free_blocks;

static void add_boot_slab_mem(unsigned long base, size_t size);
static void inline add_slab_to_slab_pool(struct slab_header *header,
		struct slab_pool *pool);

static struct slab_pool slab_pool[] = {
	{16, 	NULL, 0},
	{32, 	NULL, 0},
	{48, 	NULL, 0},
	{64, 	NULL, 0},
	{80, 	NULL, 0},
	{96, 	NULL, 0},
	{112, 	NULL, 0},
	{128, 	NULL, 0},
	{144, 	NULL, 0},
	{160, 	NULL, 0},
	{176, 	NULL, 0},
	{192, 	NULL, 0},
	{208, 	NULL, 0},
	{224, 	NULL, 0},
	{240, 	NULL, 0},
	{256, 	NULL, 0},
	{272, 	NULL, 0},
	{288, 	NULL, 0},
	{304, 	NULL, 0},
	{320, 	NULL, 0},
	{336, 	NULL, 0},
	{352, 	NULL, 0},
	{368, 	NULL, 0},
	{384, 	NULL, 0},
	{400, 	NULL, 0},
	{416, 	NULL, 0},
	{432, 	NULL, 0},
	{448, 	NULL, 0},
	{464, 	NULL, 0},
	{480, 	NULL, 0},
	{496, 	NULL, 0},
	{512, 	NULL, 0},
	{0, 	NULL, 0}		/* size > 512 will store here */
};

static LIST_HEAD(host_list);

#define PAGE_ADDR(base, bit)		(base + (bit << PAGE_SHIFT))

#define SLAB_MIN_DATA_SIZE		(16)
#define SLAB_MIN_DATA_SIZE_SHIFT	(4)
#define SLAB_HEADER_SIZE		sizeof(struct slab_header)
#define SLAB_MIN_SIZE			(SLAB_MIN_DATA_SIZE + SLAB_HEADER_SIZE)
#define SLAB_MAGIC			(0xdeadbeef)
#define SLAB_SIZE(size)			((size) + SLAB_HEADER_SIZE)
#define SLAB_HEADER_TO_ADDR(header)	\
	((void *)((unsigned long)header + SLAB_HEADER_SIZE))
#define ADDR_TO_SLAB_HEADER(base) \
	((struct slab_header *)((unsigned long)base - SLAB_HEADER_SIZE))

static int add_memory_section(unsigned long mem_base, size_t size)
{
	struct mem_section *ms;
	unsigned long mem_end;
	size_t real_size;

	if (nr_sections >= MAX_MEM_SECTIONS)
		return -EINVAL;

	/*
	 * Fix Me : may waste some memory
	 */
	ms = &mem_sections[nr_sections];
	spin_lock_init(&ms->lock);
	mem_end = ALIGN(mem_base + size, MEM_BLOCK_SIZE);
	mem_base = BALIGN(mem_base, MEM_BLOCK_SIZE);
	real_size = mem_end - mem_base;

	if (real_size == 0)
		return -EINVAL;

	ms->phy_base = mem_base;
	ms->size = real_size;
	ms->nr_blocks = real_size >> MEM_BLOCK_SHIFT;
	ms->free_blocks = ms->nr_blocks;
	ms->id = nr_sections;
	free_blocks += ms->nr_blocks;

	nr_sections++;
	pr_info("add memory section start:0x%x size:0x%x\n", mem_base, size);

	/*
	 * TBD : need to map all host memory before
	 * malloc is called
	 */
	size = size - real_size;
	if (size > (SLAB_MIN_SIZE))
		add_boot_slab_mem(mem_base + real_size, size);

	return 0;
}

static void parse_system_regions(void)
{
	int i;
	struct memtag *tag;
	size_t size = mv_config->nr_memtag;
	struct memtag *memtags = mv_config->memtags;

	for (i = 0; i < size; i++) {
		tag = &memtags[i];
		if (tag->vmid != VMID_HOST)
			continue;

		add_memory_section(tag->mem_base,
				tag->mem_end - tag->mem_base + 1);
	}
}

static unsigned long blocks_bitmap_init(unsigned long base)
{
	int i;
	struct mem_section *section;
	unsigned long bitmap_base = base;

	/*
	 * allocate memory for bitmap
	 */
	for (i = 0; i < nr_sections; i++) {
		section = &mem_sections[i];
		section->bitmap = (unsigned long *)bitmap_base;
		section->bm_current = 0;
		section->bm_end = section->nr_blocks;
		bitmap_base += BITS_TO_LONGS(section->nr_blocks) *
			sizeof(unsigned long);
	}

	memset((void *)base, 0, bitmap_base - base);

	return bitmap_base;
}

static unsigned long blocks_table_init(unsigned long base)
{
	int i;
	struct mem_section *section;
	unsigned long tmp = base;

	/*
	 * allocate memory for mem_block table need confirm
	 */
	for (i = 0; i < nr_sections; i++) {
		section = &mem_sections[i];
		section->blocks = (struct mem_block *)tmp;
		tmp += (section->nr_blocks) * sizeof(struct mem_block);
		tmp = BALIGN(tmp, sizeof(unsigned long));
	}

	memset((void *)base, 0, tmp - base);

	return tmp;
}

static int mem_sections_init(void)
{
	int i;
	size_t boot_block_size;
	unsigned long mem_start, mem_end;
	struct mem_section *section = NULL;
	struct mem_section tmp_section;
	struct mem_section *boot_section = NULL;
	unsigned long code_base = (unsigned long)&__code_start;
	struct mem_block *block;

	mem_start = (unsigned long)&__code_end;
	pr_info("minos : code_start:0x%x code_end:0x%x\n",
			code_base, mem_start);

	mem_start = BALIGN(mem_start, sizeof(unsigned long));
	mem_start = blocks_table_init(mem_start);

	mem_start = BALIGN(mem_start, sizeof(unsigned long));
	mem_start = blocks_bitmap_init(mem_start);
	pr_info("minos : code_start after blocks_init:0x%x\n", mem_start);

	/*
	 * find the boot section, boot section do not need
	 * to map again
	 */
	for (i = 0; i < nr_sections; i++) {
		section = &mem_sections[i];
		if ((code_base >= section->phy_base) && (code_base <
				(section->phy_base + section->size))) {
			boot_section = section;
			break;
		}
	}

	if (!boot_section)
		panic("errors in memory config\n");

	/*
	 * boot section must be the first entry
	 * since when map the memory need to allocate
	 * the page table, at that time only boot section
	 * has been mapped
	 */
	if (boot_section->id != 0) {
		i = boot_section->id;
		memset(&tmp_section, 0, sizeof(struct mem_section));
		memcpy(&tmp_section, &mem_sections[0], sizeof(struct mem_section));
		memcpy(&mem_sections[0], boot_section, sizeof(struct mem_section));
		memcpy(&mem_sections[i], &tmp_section, sizeof(struct mem_section));
		mem_sections[i].id = i;
		mem_sections[0].id = 0;
	}

	mem_end = BALIGN(mem_start, MEM_BLOCK_SIZE);
	pr_info("minos : free_memory_start:0x%x\n", mem_end);

	section = &mem_sections[0];
	boot_block_size = (mem_end - code_base) >> MEM_BLOCK_SHIFT;
	free_blocks -= boot_block_size;
	block = section->blocks;
	for (i = 0; i < boot_block_size; i++) {
		set_bit(i, section->bitmap);
		section->bm_current = i + 1;
		init_list(&block->list);
		block->free_pages = 0;
		block->vmid = VMID_HOST;
		block->bm_current = 0;
		block->phy_base = code_base;
		bitmap_set(block->pages_bitmap, PAGES_IN_BLOCK, 0);
		block++;
		code_base += MEM_BLOCK_SIZE;
	}

	/*
	 * put this page to the slab allocater do not
	 * waste the memory
	 */
	boot_block_size = mem_end - mem_start;
	if (boot_block_size > SLAB_MIN_SIZE )
		add_boot_slab_mem(mem_start, boot_block_size);

	return 0;
}

static void map_memory_sections(void)
{
	int i;
	struct mem_section *section;

	if (nr_sections == 1)
		return;

	/* boot section do not to be mapped again */
	for (i = 1; i < nr_sections; i++) {
		section = &mem_sections[i];
		map_host_memory(section->phy_base, section->phy_base,
				section->size, MEM_TYPE_NORMAL);
	}
}

static struct mem_section *addr_to_mem_section(unsigned long addr)
{
	int i;
	struct mem_section *section = NULL, *temp;

	for (i = 0; i < nr_sections; i++) {
		temp = &mem_sections[i];
		if ((addr >= temp->phy_base) &&
			(addr < (temp->phy_base + temp->size)))
			section = temp;
	}

	return section;
}

static struct mem_block *addr_to_mem_block(unsigned long addr)
{
	struct mem_section *section = NULL;

	section = addr_to_mem_section(addr);

	return (&section->blocks[addr >> MEM_BLOCK_SHIFT]);
}

static inline struct mem_section *block_to_mem_section(struct mem_block *block)
{
	return addr_to_mem_section(block->phy_base);
}

static inline unsigned long
offset_in_block_bitmap(unsigned long addr, struct mem_block *block)
{
	return (addr - block->phy_base) >> PAGE_SHIFT;
}

static inline unsigned long
offset_in_section_bitmap(unsigned long addr, struct mem_section *section)
{
	return (addr - section->phy_base) >> MEM_BLOCK_SHIFT;
}

static struct mem_block *
__alloc_mem_block(struct mem_section *section, unsigned long f)
{
	unsigned long bit;
	struct mem_block *block;
	unsigned long flags;

	/*
	 * fix me: use spin_lock_irqsave or spin_lock ?
	 *
	 * TBD: using spin_try_lock, if can not get the
	 * lock then switch to next section
	 */
	spin_lock_irqsave(&section->lock, flags);
	if (section->free_blocks == 0) {
		spin_unlock_irqrestore(&section->lock, flags);
		return NULL;
	}

again:
	bit = find_next_zero_bit(section->bitmap, section->nr_blocks,
			section->bm_current);
	if (bit >= section->nr_blocks) {
		if (section->bm_current != 0) {
			section->bm_current = 0;
			goto again;
		}
	}

	if (bit >= section->nr_blocks) {
		pr_warn("section->free_blocks may be incorrect %d\n",
				section->free_blocks);
		spin_unlock_irqrestore(&section->lock, flags);
		return NULL;
	}

	section->free_blocks--;
	section->bm_current++;
	if (section->bm_current >= section->nr_blocks)
		section->bm_current = 0;

	/*
	 * can release the spin lock after set the related
	 * bit to 1
	 */
	set_bit(bit, section->bitmap);
	free_blocks--;
	spin_unlock_irqrestore(&section->lock, flags);

	block = &section->blocks[bit];
	memset(block, 0, sizeof(struct mem_block));
	block->free_pages = PAGES_IN_BLOCK;
	block->flags = f & GFB_MASK;
	block->vmid = VMID_HOST;
	block->phy_base = section->phy_base + bit * MEM_BLOCK_SIZE;

	return block;
}

struct mem_block *alloc_mem_block(unsigned long flags)
{
	int i;
	struct mem_block *block = NULL;
	struct mem_section *section;

	for (i = 0; i < nr_sections; i++) {
		section = &mem_sections[i];
		block = __alloc_mem_block(section, flags);
		if (block)
			break;
	}

	return block;
}

static unsigned long *get_page_meta(void)
{
	int bit;
	struct mem_block *block = NULL, *n = NULL;

	list_for_each_entry_safe(block, n, &page_pool->meta_list, list) {
again:
		bit = find_next_zero_bit(block->pages_bitmap,
				PAGE_METAS_IN_BLOCK, block->bm_current);
		if (bit >= PAGE_METAS_IN_BLOCK) {
			if (block->bm_current != 0) {
				block->bm_current = 0;
				goto again;
			} else {
				pr_error("block meta free_pages is not correct\n");
				list_del(&block->list);
				continue;
			}
		}

		block->free_pages--;
		if (block->free_pages == 0)
			list_del(&block->list);

		block->bm_current = bit + 1;
		if (block->bm_current == PAGE_METAS_IN_BLOCK)
			block->bm_current = 0;
		set_bit(bit, block->pages_bitmap);

		return (unsigned long *)(block->phy_base + bit * PAGE_META_SIZE);
	}

	block = alloc_mem_block(GPF_PAGE_META);
	if (!block)
		return NULL;

	block->free_pages = PAGE_METAS_IN_BLOCK - 1;
	block->pages_bitmap = (unsigned long *)(block->phy_base +
			MEM_BLOCK_SIZE - PAGE_METAS_BITMAP_SIZE);
	memset(block->pages_bitmap, 0, PAGE_METAS_BITMAP_SIZE);
	set_bit(0, block->pages_bitmap);
	block->bm_current = 1;
	list_add(&page_pool->meta_list, &block->list);
	page_pool->meta_blocks++;

	return (unsigned long *)block->phy_base;
}

static inline void *block_meta_base(struct mem_block *block)
{
	return ((void *)block->pages_bitmap + BLOCK_BITMAP_SIZE);
}

static struct page *alloc_pages_from_block(struct mem_block *block,
		int count, int align)
{
	int bit, i;
	void *addr;
	struct page *meta, *page;

	if (block->free_pages < count)
		return NULL;

	if (count == 1) {
again:
		bit = find_next_zero_bit(block->pages_bitmap,
				PAGES_IN_BLOCK, block->bm_current);
		if (bit >= PAGES_IN_BLOCK) {
			if (block->bm_current != 0) {
				block->bm_current = 0;
				goto again;
			}
		}
	} else {
		bit = bitmap_find_next_zero_area_align(block->pages_bitmap,
				PAGES_IN_BLOCK, 0, count, align);
	}

	if (bit >= PAGES_IN_BLOCK)
		return NULL;

	/*
	 * update the meta info for this block
	 */
	block->free_pages -= count;
	bitmap_set(block->pages_bitmap, bit, count);
	if (count == 1) {
		block->bm_current = bit + count;
		if (block->bm_current == PAGES_IN_BLOCK)
			block->bm_current = 0;
	}

	meta = (struct page *)block_meta_base(block);
	meta += bit;
	page = meta;
	addr = (void *)PAGE_ADDR(block->phy_base, bit);
	meta->phy_base = (unsigned long)addr | (count & 0xfff);
	for (i = 1; i < count; i++) {
		meta++;
		meta->phy_base = (unsigned long)(addr + PAGE_SIZE * i) | 0xfff;
	}

	return page;
}

struct page *__alloc_pages(int count, int align)
{
	struct mem_block *block = NULL, *n = NULL;
	unsigned long f;
	unsigned long *page_meta = NULL;
	struct page *page = NULL;

	if (count <= 0)
		return NULL;

	spin_lock_irqsave(&page_pool->lock, f);

	list_for_each_entry_safe(block, n, &page_pool->block_list, list) {
		page = alloc_pages_from_block(block, count, align);
		if (page) {
			if (block->free_pages == 0)
				list_del(&block->list);
			break;
		}
	}

	/*
	 * need new memory block from the section
	 */
	if (page)
		goto out;

	block = alloc_mem_block(GFB_PAGE);
	if (!block)
		goto out;

	page_meta = get_page_meta();
	if (!page_meta)
		goto out;

	memset(page_meta, 0, PAGE_META_SIZE);
	block->pages_bitmap = page_meta;
	list_add(&page_pool->block_list, &block->list);

	page = alloc_pages_from_block(block, count, align);

out:
	spin_unlock_irqrestore(&page_pool->lock, f);
	return page;
}

void *__get_free_pages(int pages, int align)
{
	struct page *page = __alloc_pages(pages, align);

	if (page)
		return (void *)(page->phy_base & __PAGE_MASK);

	return NULL;
}

static size_t inline get_slab_alloc_size(size_t size)
{
	return BALIGN(size, SLAB_MIN_DATA_SIZE);
}

static inline int slab_pool_id(size_t size)
{
	int id = (size >> SLAB_MIN_DATA_SIZE_SHIFT) - 1;

	return id >= pslab->pool_nr ? (pslab->pool_nr - 1) : id;
}

static void add_boot_slab_mem(unsigned long base, size_t size)
{
	int i;
	struct slab_pool *pool;
	struct slab_header *header;

	pr_info("add boot_mem-0x%x : 0x%x to slab\n", base, size);

	/*
	 * this function will be only called on boot
	 * time and on the cpu 0, so do not need to
	 * aquire the spin lock.
	 */
	if ((base + size) & (MEM_BLOCK_SIZE - 1))
		pr_warn("memory may be a block\n");

	while (size >= SLAB_MIN_SIZE) {
		for (i = (pslab->pool_nr - 2); i >= 0; i--) {
			pool = &pslab->pool[i];
			if (size < (pool->size + SLAB_HEADER_SIZE))
				continue;

			header = (struct slab_header *)base;
			header->size = pool->size;
			header->next = NULL;
			add_slab_to_slab_pool(header, pool);
			base += pool->size + SLAB_HEADER_SIZE;
			size -= pool->size + SLAB_HEADER_SIZE;

			if (size < SLAB_MIN_SIZE)
				break;
		}
	}
}

static int alloc_new_slab_block(void)
{
	struct mem_block *block;

	block = alloc_mem_block(GFB_SLAB);
	if (!block)
		return -ENOMEM;

	list_add_tail(&pslab->block_list, &block->list);

	/*
	 * in slab allocator free_pages is ued to count how
	 * many slabs has been allocated out, if the count
	 * is 1, then this block can return back tho the
	 * section
	 */
	block->free_pages = 1;
	pslab->free_size = MEM_BLOCK_SIZE;
	pslab->slab_free = block->phy_base;
	pslab->alloc_blocks++;

	return 0;
}

static inline struct slab_header *
get_slab_from_slab_pool(struct slab_pool *pool)
{
	struct slab_header *header;

	header = pool->head;
	pool->head = header->next;
	header->magic = SLAB_MAGIC;

	return header;
}

static void inline add_slab_to_slab_pool(struct slab_header *header,
		struct slab_pool *pool)
{
	header->next = pool->head;
	pool->head = header;
}

static void *get_slab_from_pool(size_t size)
{
	struct slab_pool *slab_pool;
	struct slab_header *header;
	int id = slab_pool_id(size);

	slab_pool = &pslab->pool[id];
	if (!slab_pool->head)
		return NULL;

	header = get_slab_from_slab_pool(slab_pool);

	return SLAB_HEADER_TO_ADDR(header);
}

static void *get_slab_from_slab_free(size_t size)
{
	struct slab_header *header;
	size_t request_size = SLAB_SIZE(size);
	struct mem_block *block;

	if (pslab->free_size < request_size)
		return NULL;

	header = (struct slab_header *)pslab->slab_free;
	memset(header, 0, sizeof(struct slab_header));
	header->size = size;
	header->magic = SLAB_MAGIC;

	block = addr_to_mem_block(pslab->slab_free);
	pslab->slab_free += request_size;
	pslab->free_size -= request_size;

	if (pslab->free_size < SLAB_SIZE(SLAB_MIN_DATA_SIZE)) {
		header->size += pslab->free_size;
		pslab->slab_free = 0;
		pslab->free_size = 0;
	}

	block->free_pages++;

	return SLAB_HEADER_TO_ADDR(header);
}

static void *get_new_slab(size_t size)
{
	int id;
	struct slab_pool *pool;
	struct slab_header *header;
	struct mem_block *block;
	static int times = 0;

	if (pslab->alloc_blocks > CONFIG_MAX_SLAB_BLOCKS) {
		if (times == 0)
			pr_warn("slab pool block size is bigger than %d\n",
					CONFIG_MAX_SLAB_BLOCKS);
		times++;
		return NULL;
	}

	if (pslab->free_size >= SLAB_SIZE(SLAB_MIN_DATA_SIZE)) {
		id = slab_pool_id(pslab->free_size - SLAB_HEADER_SIZE);
		pool = &pslab->pool[id];

		block = addr_to_mem_block(pslab->slab_free);

		header = (struct slab_header *)pslab->slab_free;
		memset(header, 0, SLAB_HEADER_SIZE);
		header->size = pslab->free_size - SLAB_HEADER_SIZE;
		header->magic = SLAB_MAGIC;
		add_slab_to_slab_pool(header, pool);

		pslab->free_size = 0;
		pslab->slab_free = 0;

		block->free_pages++;
	}

	if (alloc_new_slab_block())
		return NULL;

	return get_slab_from_slab_free(size);
}

static void *get_big_slab(size_t size)
{
	struct slab_pool *slab_pool;
	struct slab_header *header;
	int id = slab_pool_id(size);

	while (1) {
		slab_pool = &pslab->pool[id];
		if (!slab_pool->head) {
			id++;
			if (id == pslab->pool_nr)
				return NULL;
		} else {
			break;
		}
	}

	header = (struct slab_header *)get_slab_from_slab_pool(slab_pool);

	return SLAB_HEADER_TO_ADDR(header);
}

typedef void *(*slab_alloc_func)(size_t size);

static slab_alloc_func alloc_func[] = {
	get_slab_from_pool,
	get_slab_from_slab_free,
	get_new_slab,
	get_big_slab,
	NULL,
};

void *malloc(size_t size)
{
	int i = 0;
	void *ret = NULL;
	slab_alloc_func func;
	unsigned long flags;

	if (size == 0)
		return NULL;

	size = get_slab_alloc_size(size);
	spin_lock_irqsave(&pslab->lock, flags);

	while (1) {
		func = alloc_func[i];
		if (!func)
			break;

		ret = func(size);
		if (ret)
			break;
		i++;
	}

	spin_unlock_irqrestore(&pslab->lock, flags);

	return ret;
}

void *zalloc(size_t size)
{
	void *base;

	base = malloc(size);
	if (!base)
		return NULL;

	memset(base, 0, size);
	return base;
}

void release_mem_block(struct mem_block *block)
{
	unsigned long start;
	unsigned long flags = 0;
	struct mem_section *section = NULL;
	struct mem_block *meta_block = NULL;

	if (!block)
		return;

	if (block->free_pages < PAGES_IN_BLOCK)
		goto out;

	section = block_to_mem_section(block);
	spin_lock_irqsave(&section->lock, flags);
	start = offset_in_section_bitmap(block->phy_base, section);
	bitmap_clear(section->bitmap, start, 1);
	free_blocks++;

	/*
	 * shoud do this here or in free_page function ?
	 * TBD fix me
	 */
	if (block->flags & BIT(GFB_PAGE_BIT)) {
		meta_block = addr_to_mem_block((unsigned long)block->pages_bitmap);
		if (!meta_block)
			goto out;
		start = ((unsigned long)(block->pages_bitmap) -
				block->phy_base) / PAGE_META_SIZE;
		clear_bit(start, meta_block->pages_bitmap);
	}
out:
	spin_unlock_irqrestore(&section->lock, flags);
}

void free_pages(void *addr)
{
	struct mem_block *block;
	unsigned long start;
	unsigned long flags;
	struct page *meta;
	int i, count;

	block = addr_to_mem_block((unsigned long)addr);
	start = offset_in_block_bitmap((unsigned long)addr, block);

	if (!(block->flags & BIT(GFB_PAGE_BIT))) {
		pr_error("addr is not a page 0x%p\n", addr);
		return;
	}

	spin_lock_irqsave(&page_pool->lock, flags);

	meta = (struct page *)block_meta_base(block);
	count = meta->phy_base & 0xfff;
	i = block->free_pages;
	block->free_pages += count;

	/*
	 * add the block to the page pool if the
	 * pages free is not 0
	 */
	if (i == 0)
		list_add_tail(&page_pool->block_list, &block->list);

	bitmap_clear(block->pages_bitmap, start, count);
	for (i = start; i < count; i++) {
		meta->phy_base = 0;
		meta++;
	}

	spin_unlock_irqrestore(&page_pool->lock, flags);
}

void release_pages(struct page *page)
{
	return free_pages((void *)(page->phy_base & __PAGE_MASK));
}

void free(void *addr)
{
	struct slab_header *header;
	struct slab_pool *slab_pool;
	unsigned long flags;
	struct mem_block *block;

	if (!addr)
		return;

	block = addr_to_mem_block((unsigned long)addr);
	if (block->flags & BIT(GFB_PAGE_BIT)) {
		free_pages(addr);
		return;
	}

	header = ADDR_TO_SLAB_HEADER(addr);
	if (header->magic != SLAB_MAGIC)
		return;

	spin_lock_irqsave(&pslab->lock, flags);
	slab_pool = &pslab->pool[slab_pool_id(header->size)];
	add_slab_to_slab_pool(header, slab_pool);
	spin_unlock_irqrestore(&pslab->lock, flags);
}

static void slab_init(void)
{
	pr_info("Slab memory allocator init...\n");
	memset(pslab, 0, sizeof(struct slab));
	init_list(&pslab->block_list);
	pslab->pool = slab_pool;
	pslab->pool_nr = sizeof(slab_pool) / sizeof(slab_pool[0]);
	spin_lock_init(&pslab->lock);
}

static void page_pool_init(void)
{
	memset(page_pool, 0, sizeof(struct page_pool));
	spin_lock_init(&page_pool->lock);
	init_list(&page_pool->meta_list);
	init_list(&page_pool->block_list);
}

int has_enough_memory(size_t size)
{
	return (free_blocks >= (size >> MEM_BLOCK_SHIFT));
}

int mm_init(void)
{
	size_t size;

	pr_info("dynamic memory allocator init..\n");

	nr_sections = 0;
	size = sizeof(struct mem_section) * MAX_MEM_SECTIONS;
	memset(mem_sections, 0, size);

	/*
	 * first need init the slab allocator then
	 * parsing the memroy region to the hypervisor
	 * and convert it to the memory section
	 */
	slab_init();
	page_pool_init();
	parse_system_regions();
	mem_sections_init();
	map_memory_sections();

	return 0;
}
