/*
The MIT License (MIT)
Copyright (c) 2014-2015 CSAIL, MIT
Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:
The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.
THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
*/

#if defined (KERNEL_MODE)
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/log2.h>

#elif defined (USER_MODE)
#include <stdio.h>
#include <stdint.h>
#include "uilog.h"
#include "upage.h"

#else
#error Invalid Platform (KERNEL_MODE or USER_MODE)
#endif

#include "bdbm_drv.h"
#include "params.h"
#include "debug.h"
#include "utime.h"
#include "ufile.h"
#include "umemory.h"
#include "hlm_reqs_pool.h"
#include "uthash.h"

#include "algo/abm.h"
#include "algo/fgm_ftl.h"

/* FTL interface */
bdbm_ftl_inf_t _ftl_fgm_ftl = {
    .ptr_private = NULL,
    .create = bdbm_fgm_ftl_create,
    .destroy = bdbm_fgm_ftl_destroy,
    .get_free_ppa = bdbm_fgm_ftl_get_free_ppa,
    .get_ppa = bdbm_fgm_ftl_get_ppa,
    .map_lpa_to_ppa = bdbm_fgm_ftl_map_lpa_to_ppa,
    .invalidate_lpa = bdbm_fgm_ftl_invalidate_lpa,
    .do_gc = bdbm_fgm_ftl_do_gc,
    .is_gc_needed = bdbm_fgm_ftl_is_gc_needed,
    .scan_badblocks = bdbm_fgm_badblock_scan,
};


/* data structures for block-level FTL */
enum BDBM_PFTL_PAGE_STATUS {
    PFTL_PAGE_NOT_ALLOCATED = 0,
    PFTL_PAGE_VALID,
    PFTL_PAGE_INVALID,
    PFTL_PAGE_INVALID_ADDR = -1ULL,
};

typedef struct {
    uint8_t status; /* BDBM_PFTL_PAGE_STATUS */
    bdbm_phyaddr_t phyaddr; /* physical location */
    uint8_t sp_off;
} bdbm_fgm_mapping_entry_t;

typedef struct bdbm_fgm_sp_mapping_entry_t{
    uint64_t lpa;
    bdbm_phyaddr_t ppa;
    uint8_t sp_off;
    UT_hash_handle hh;
} bdbm_fgm_sp_mapping_entry_t;

typedef struct {
    bdbm_abm_info_t* bai;
    bdbm_fgm_mapping_entry_t* ptr_mapping_table;
    bdbm_fgm_sp_mapping_entry_t* ptr_sp_hash_table;

    bdbm_spinlock_t ftl_lock;
    uint64_t nr_punits;
    uint64_t nr_punits_pages;
    uint64_t nr_max_dirty_4kb_blks;

    /* for the management of active blocks */
    uint64_t curr_puid;
    uint64_t curr_page_ofs;
    bdbm_abm_block_t** ac_bab;

    /* for the management of 4KB active blocks */
    uint64_t curr_puid_4kb;
    uint64_t curr_page_ofs_4kb;
    bdbm_abm_block_t** ac_bab_4kb;


    /* reserved for gc (reused whenever gc is invoked) */
    bdbm_abm_block_t** gc_bab;
    bdbm_hlm_req_gc_t gc_hlm;
    bdbm_hlm_req_gc_t gc_hlm_w;

    /* for bad-block scanning */
    bdbm_sema_t badblk;
} bdbm_fgm_ftl_private_t;

bdbm_fgm_sp_mapping_entry_t* find_lpa_4kb(bdbm_fgm_ftl_private_t* p, uint64_t lpa)
{
    struct bdbm_fgm_sp_mapping_entry_t *s;

    HASH_FIND_INT(p->ptr_sp_hash_table, &lpa, s);
    return s;
}


void update_lpa_4kb(bdbm_fgm_ftl_private_t* p, uint64_t lpa, bdbm_phyaddr_t* ppa, uint8_t sp_off)
{
    struct bdbm_fgm_sp_mapping_entry_t *s;

    HASH_FIND_INT(p->ptr_sp_hash_table, &lpa, s);
    if (s==NULL) {
        s = (struct bdbm_fgm_sp_mapping_entry_t*)bdbm_malloc(sizeof(struct bdbm_fgm_sp_mapping_entry_t));
        s->lpa = lpa;
        HASH_ADD_INT(p->ptr_sp_hash_table, lpa, s );
    }
    s->ppa.punit_id = ppa->punit_id;
    s->ppa.channel_no = ppa->channel_no;
    s->ppa.chip_no = ppa->chip_no;
    s->ppa.block_no = ppa->block_no;
    s->ppa.page_no = ppa->page_no;
    s->sp_off = sp_off;

}

void invalidate_lpa_4kb(bdbm_fgm_ftl_private_t* p, uint64_t lpa)
{
    struct bdbm_fgm_sp_mapping_entry_t *s;

    HASH_FIND_INT(p->ptr_sp_hash_table, &lpa, s);
    if (s != NULL) {
       HASH_DEL(p->ptr_sp_hash_table, s);
       bdbm_free(s); 
    } 
}


bdbm_fgm_mapping_entry_t* __bdbm_fgm_ftl_create_mapping_table (
        bdbm_device_params_t* np)
{
    bdbm_fgm_mapping_entry_t* me;
    uint64_t loop;

    /* create a page-level mapping table */
    if ((me = (bdbm_fgm_mapping_entry_t*)bdbm_zmalloc 
                (sizeof (bdbm_fgm_mapping_entry_t) *
                 (np->nr_subpages_per_ssd / np->nr_subpages_per_page))) == NULL) {
        return NULL;
    }

    /* initialize a page-level mapping table */
    for (loop = 0; loop < np->nr_pages_per_ssde; loop++) {
        me[loop].status = PFTL_PAGE_NOT_ALLOCATED;
        me[loop].phyaddr.channel_no = PFTL_PAGE_INVALID_ADDR;
        me[loop].phyaddr.chip_no = PFTL_PAGE_INVALID_ADDR;
        me[loop].phyaddr.block_no = PFTL_PAGE_INVALID_ADDR;
        me[loop].phyaddr.page_no = PFTL_PAGE_INVALID_ADDR;
        me[loop].sp_off = -1;
    }

    /* return a set of mapping entries */
    return me;
}


void __bdbm_fgm_ftl_destroy_mapping_table (
        bdbm_fgm_mapping_entry_t* me)
{
    if (me == NULL)
        return;
    bdbm_free (me);
}

uint32_t __bdbm_fgm_ftl_get_active_blocks (
        bdbm_device_params_t* np,
        bdbm_abm_info_t* bai,
        bdbm_abm_block_t** bab)
{
    uint64_t i, j;

    /* get a set of free blocks for active blocks */
    for (i = 0; i < np->nr_channels; i++) {
        for (j = 0; j < np->nr_chips_per_channel; j++) {
            /* prepare & commit free blocks */
            if ((*bab = bdbm_abm_get_free_block_prepare (bai, i, j))) {
                bdbm_abm_get_free_block_commit (bai, *bab);
                /*bdbm_msg ("active blk = %p", *bab);*/
                bab++;
            } else {
                bdbm_error ("bdbm_abm_get_free_block_prepare failed");
                return 1;
            }
        }
    }

    return 0;
}

bdbm_abm_block_t** __bdbm_fgm_ftl_create_active_blocks_4kb (
        bdbm_device_params_t* np,
        bdbm_abm_info_t* bai)
{
    uint64_t nr_punits;
    bdbm_abm_block_t** bab_4kb = NULL;

    nr_punits = np->nr_chips_per_channel * np->nr_channels;

    /* create a set of active blocks */
    if ((bab_4kb = (bdbm_abm_block_t**)bdbm_zmalloc 
                (sizeof (bdbm_abm_block_t*) * nr_punits)) == NULL) {
        bdbm_error ("bdbm_zmalloc failed");
        goto fail;
    }

    /* get a set of free blocks for active blocks */
    if (__bdbm_fgm_ftl_get_active_blocks (np, bai, bab_4kb) != 0) {
        bdbm_error ("__bdbm_fgm_ftl_get_active_blocks failed");
        goto fail;
    }

    return bab_4kb;

fail:
    if (bab_4kb)
        bdbm_free (bab_4kb);
    return NULL;
}


bdbm_abm_block_t** __bdbm_fgm_ftl_create_active_blocks (
        bdbm_device_params_t* np,
        bdbm_abm_info_t* bai)
{
    uint64_t nr_punits;
    bdbm_abm_block_t** bab = NULL;

    nr_punits = np->nr_chips_per_channel * np->nr_channels;

    /* create a set of active blocks */
    if ((bab = (bdbm_abm_block_t**)bdbm_zmalloc 
                (sizeof (bdbm_abm_block_t*) * nr_punits)) == NULL) {
        bdbm_error ("bdbm_zmalloc failed");
        goto fail;
    }

    /* get a set of free blocks for active blocks */
    if (__bdbm_fgm_ftl_get_active_blocks (np, bai, bab) != 0) {
        bdbm_error ("__bdbm_fgm_ftl_get_active_blocks failed");
        goto fail;
    }

    return bab;

fail:
    if (bab)
        bdbm_free (bab);
    return NULL;
}

void __bdbm_fgm_ftl_destroy_active_blocks (
        bdbm_abm_block_t** bab)
{
    if (bab == NULL)
        return;

    /* TODO: it might be required to save the status of active blocks 
     * in order to support rebooting */
    bdbm_free (bab);
}

uint32_t bdbm_fgm_ftl_create (bdbm_drv_info_t* bdi)
{
    bdbm_fgm_ftl_private_t* p = NULL;
    bdbm_device_params_t* np = BDBM_GET_DEVICE_PARAMS (bdi);

    /* create a private data structure */
    if ((p = (bdbm_fgm_ftl_private_t*)bdbm_zmalloc 
                (sizeof (bdbm_fgm_ftl_private_t))) == NULL) {
        bdbm_error ("bdbm_malloc failed");
        return 1;
    }
    p->curr_puid = 0;
    p->curr_page_ofs = 0;
    p->curr_puid_4kb = 0;
    p->curr_page_ofs_4kb = 0;

    p->nr_punits = np->nr_chips_per_channel * np->nr_channels;
    p->nr_punits_pages = p->nr_punits * np->nr_pages_per_block;
    p->nr_max_dirty_4kb_blks = p->nr_punits * np->nr_blocks_per_chip / 3;


    bdbm_spin_lock_init (&p->ftl_lock);
    _ftl_fgm_ftl.ptr_private = (void*)p;

    /* create 'bdbm_abm_info' with pst */
    if ((p->bai = bdbm_abm_create (np, 1)) == NULL) {
        bdbm_error ("bdbm_abm_create failed");
        bdbm_fgm_ftl_destroy (bdi);
        return 1;
    }

    /* create a sub page hash table. it only need to set NULL */
    p->ptr_sp_hash_table = NULL;

    /* create a mapping table */
    if ((p->ptr_mapping_table = __bdbm_fgm_ftl_create_mapping_table (np)) == NULL) {
        bdbm_error ("__bdbm_fgm_ftl_create_mapping_table failed");
        bdbm_fgm_ftl_destroy (bdi);
        return 1;
    }

    /* allocate active blocks */
    if ((p->ac_bab = __bdbm_fgm_ftl_create_active_blocks (np, p->bai)) == NULL) {
        bdbm_error ("__bdbm_fgm_ftl_create_active_blocks failed");
        bdbm_fgm_ftl_destroy (bdi);
        return 1;
    }

    /* allocate 4kb active blocks */
    if ((p->ac_bab_4kb = __bdbm_fgm_ftl_create_active_blocks (np, p->bai)) == NULL) {
        bdbm_error ("__bdbm_fgm_ftl_create_active_blocks failed");
        bdbm_fgm_ftl_destroy (bdi);
        return 1;
    }

    /* allocate gc stuff */
    if ((p->gc_bab = (bdbm_abm_block_t**)bdbm_zmalloc 
                (sizeof (bdbm_abm_block_t*) * p->nr_punits)) == NULL) {
        bdbm_error ("bdbm_zmalloc failed");
        bdbm_fgm_ftl_destroy (bdi);
        return 1;
    }

    if ((p->gc_hlm.llm_reqs = (bdbm_llm_req_t*)bdbm_zmalloc
                (sizeof (bdbm_llm_req_t) * p->nr_punits_pages)) == NULL) {
        bdbm_error ("bdbm_zmalloc failed");
        bdbm_fgm_ftl_destroy (bdi);
        return 1;
    }
    bdbm_sema_init (&p->gc_hlm.done);
    hlm_reqs_pool_allocate_llm_reqs (p->gc_hlm.llm_reqs, p->nr_punits_pages, RP_MEM_PHY);

    if ((p->gc_hlm_w.llm_reqs = (bdbm_llm_req_t*)bdbm_zmalloc
                (sizeof (bdbm_llm_req_t) * p->nr_punits_pages)) == NULL) {
        bdbm_error ("bdbm_zmalloc failed");
        bdbm_fgm_ftl_destroy (bdi);
        return 1;
    }
    bdbm_sema_init (&p->gc_hlm_w.done);
    hlm_reqs_pool_allocate_llm_reqs (p->gc_hlm_w.llm_reqs, p->nr_punits_pages, RP_MEM_PHY);

    return 0;
}

void bdbm_fgm_ftl_destroy (bdbm_drv_info_t* bdi)
{
    bdbm_fgm_ftl_private_t* p = _ftl_fgm_ftl.ptr_private;

    if (!p)
        return;
    if (p->gc_hlm_w.llm_reqs) {
        hlm_reqs_pool_release_llm_reqs (p->gc_hlm_w.llm_reqs, p->nr_punits_pages, RP_MEM_PHY);
        bdbm_sema_free (&p->gc_hlm_w.done);
        bdbm_free (p->gc_hlm_w.llm_reqs);
    }
    if (p->gc_hlm.llm_reqs) {
        hlm_reqs_pool_release_llm_reqs (p->gc_hlm.llm_reqs, p->nr_punits_pages, RP_MEM_PHY);
        bdbm_sema_free (&p->gc_hlm.done);
        bdbm_free (p->gc_hlm.llm_reqs);
    }
    if (p->gc_bab)
        bdbm_free (p->gc_bab);
    if (p->ac_bab)
        __bdbm_fgm_ftl_destroy_active_blocks (p->ac_bab);
    if (p->ac_bab_4kb)
        __bdbm_fgm_ftl_destroy_active_blocks (p->ac_bab_4kb);
    if (p->ptr_mapping_table)
        __bdbm_fgm_ftl_destroy_mapping_table (p->ptr_mapping_table);
    if (p->bai)
        bdbm_abm_destroy (p->bai);
    bdbm_free (p);
}

uint32_t __bdbm_fgm_ftl_get_free_ppa_4kb (
        bdbm_drv_info_t* bdi, 
        int64_t lpa,
        bdbm_phyaddr_t* ppa)
{
    bdbm_fgm_ftl_private_t* p = _ftl_fgm_ftl.ptr_private;
    bdbm_device_params_t* np = BDBM_GET_DEVICE_PARAMS (bdi);
    bdbm_abm_block_t* b = NULL;
    uint64_t curr_channel;
    uint64_t curr_chip;

    /* get the channel & chip numbers */
    curr_channel = p->curr_puid_4kb % np->nr_channels;
    curr_chip = p->curr_puid_4kb / np->nr_channels;

    /* get the physical offset of the active blocks */
    b = p->ac_bab_4kb[curr_channel * np->nr_chips_per_channel + curr_chip];
    ppa->channel_no =  b->channel_no;
    ppa->chip_no = b->chip_no;
    ppa->block_no = b->block_no;
    ppa->page_no = p->curr_page_ofs_4kb;
    ppa->punit_id = BDBM_GET_PUNIT_ID (bdi, ppa);

    /* check some error cases before returning the physical address */
    bdbm_bug_on (ppa->channel_no != curr_channel);
    bdbm_bug_on (ppa->chip_no != curr_chip);
    bdbm_bug_on (ppa->page_no >= np->nr_pages_per_block);

    /* go to the next parallel unit */
    if ((p->curr_puid_4kb + 1) == p->nr_punits) {
        p->curr_puid_4kb = 0;
        p->curr_page_ofs_4kb++;	/* go to the next page */

        /* see if there are sufficient free pages or not */
        if (p->curr_page_ofs_4kb == np->nr_pages_per_block) {

            if(p->bai->nr_dirty_4kb_blks == p->nr_max_dirty_4kb_blks){
                /*TODO*/
                return 1;
            }
            else{
                if (__bdbm_fgm_ftl_get_active_blocks (np, p->bai, p->ac_bab_4kb) != 0) {
                    bdbm_error ("__bdbm_fgm_ftl_get_active_blocks failed");
                    return 1;
                }
            }
            /* ok; go ahead with 0 offset */
            /*bdbm_msg ("curr_puid = %llu", p->curr_puid);*/
            p->curr_page_ofs_4kb = 0;
        }
    } else {
        /*bdbm_msg ("curr_puid = %llu", p->curr_puid);*/
        p->curr_puid_4kb++;
    }

    return 0;
}

uint32_t __bdbm_fgm_ftl_get_free_ppa (
        bdbm_drv_info_t* bdi, 
        int64_t lpa,
        bdbm_phyaddr_t* ppa)
{
    bdbm_fgm_ftl_private_t* p = _ftl_fgm_ftl.ptr_private;
    bdbm_device_params_t* np = BDBM_GET_DEVICE_PARAMS (bdi);
    bdbm_abm_block_t* b = NULL;
    uint64_t curr_channel;
    uint64_t curr_chip;

    /* get the channel & chip numbers */
    curr_channel = p->curr_puid % np->nr_channels;
    curr_chip = p->curr_puid / np->nr_channels;

    /* get the physical offset of the active blocks */
    b = p->ac_bab[curr_channel * np->nr_chips_per_channel + curr_chip];
    ppa->channel_no =  b->channel_no;
    ppa->chip_no = b->chip_no;
    ppa->block_no = b->block_no;
    ppa->page_no = p->curr_page_ofs;
    ppa->punit_id = BDBM_GET_PUNIT_ID (bdi, ppa);

    /* check some error cases before returning the physical address */
    bdbm_bug_on (ppa->channel_no != curr_channel);
    bdbm_bug_on (ppa->chip_no != curr_chip);
    bdbm_bug_on (ppa->page_no >= np->nr_pages_per_block);

    /* go to the next parallel unit */
    if ((p->curr_puid + 1) == p->nr_punits) {
        p->curr_puid = 0;
        p->curr_page_ofs++;	/* go to the next page */

        /* see if there are sufficient free pages or not */
        if (p->curr_page_ofs == np->nr_pages_per_block) {
            /* get active blocks */
            if (__bdbm_fgm_ftl_get_active_blocks (np, p->bai, p->ac_bab) != 0) {
                bdbm_error ("__bdbm_fgm_ftl_get_active_blocks failed");
                return 1;
            }
            /* ok; go ahead with 0 offset */
            /*bdbm_msg ("curr_puid = %llu", p->curr_puid);*/
            p->curr_page_ofs = 0;
        }
    } else {
        /*bdbm_msg ("curr_puid = %llu", p->curr_puid);*/
        p->curr_puid++;
    }

    return 0;
}

uint32_t bdbm_fgm_ftl_get_free_ppa (
        bdbm_drv_info_t* bdi, 
        bdbm_logaddr_t* logaddr,
        bdbm_phyaddr_t* ppa)
{
    if(logaddr->lpa_cg == -1){
        return __bdbm_fgm_ftl_get_free_ppa_4kb(bdi, logaddr->lpa_cg, ppa);
    }else{
        return __bdbm_fgm_ftl_get_free_ppa(bdi, logaddr->lpa_cg, ppa);
    }
}

uint32_t __bdbm_fgm_ftl_map_lpa_to_ppa_4kb (
        bdbm_drv_info_t* bdi, 
        bdbm_logaddr_t* logaddr,
        bdbm_phyaddr_t* phyaddr)
{
    bdbm_device_params_t* np = BDBM_GET_DEVICE_PARAMS (bdi);
    bdbm_fgm_ftl_private_t* p = _ftl_fgm_ftl.ptr_private;
    bdbm_fgm_sp_mapping_entry_t* spme = NULL;
    bdbm_abm_block_t* b = bdbm_abm_get_block (p->bai,
           phyaddr->channel_no, phyaddr->chip_no, phyaddr->block_no);
    int k, pst_off = 0;


    if (logaddr->lpa[logaddr->ofs] >= np->nr_subpages_per_ssd) {
        bdbm_error ("LPA is beyond logical space (%llX)", logaddr->lpa[k]);
        return 1;
    }

    for (k = 0; k < np->nr_subpages_per_page; k++) {
        pst_off = (phyaddr->page_no * np->nr_subpages_per_page) + k;
        if(b->pst[pst_off] == BABM_ABM_SUBPAGE_NOT_INVALID)
            break;
    }
    /*
       printf("logaddr=%d, ofs=%d, phy:: ch=%d, chip=%d, block=%d, page_no=%d sub page = %d\n",
       logaddr->lpa[logaddr->ofs], logaddr->ofs,
       phyaddr->channel_no, 
       phyaddr->chip_no,
       phyaddr->block_no,
       phyaddr->page_no,
       pst_off);
       */

    spme = find_lpa_4kb(p, logaddr->lpa[logaddr->ofs]);
    if(spme != NULL){
        bdbm_abm_invalidate_page_4kb (
                p->bai, 
                spme->ppa.channel_no, 
                spme->ppa.chip_no, 
                spme->ppa.block_no, 
                spme->ppa.page_no, 
                spme->sp_off
                );
        printf("logaddr=%d, ofs=%d, phy:: ch=%d, chip=%d, block=%d, page_no=%d sub page = %d\n",
                logaddr->lpa[logaddr->ofs], logaddr->ofs,
                spme->ppa.channel_no, 
                spme->ppa.chip_no, 
                spme->ppa.block_no, 
                spme->ppa.page_no, 
                spme->sp_off);

    }
    update_lpa_4kb(p, logaddr->lpa[logaddr->ofs], phyaddr, k);
    //spme = find_lpa_4kb(p, logaddr->lpa[logaddr->ofs]); //for debug

    return 0;
}

uint32_t __bdbm_fgm_ftl_map_lpa_to_ppa (
        bdbm_drv_info_t* bdi, 
        bdbm_logaddr_t* logaddr,
        bdbm_phyaddr_t* phyaddr)
{
    bdbm_device_params_t* np = BDBM_GET_DEVICE_PARAMS (bdi);
    bdbm_fgm_ftl_private_t* p = _ftl_fgm_ftl.ptr_private;
    bdbm_fgm_mapping_entry_t* me = NULL;
    bdbm_fgm_sp_mapping_entry_t* spme = NULL;
    int k = 0;

    /* is it a valid logical address */
    /* printf("logaddr=%d, phy:: ch=%d, chip=%d, block=%d, page_no=%d\n",
       logaddr->lpa_cg,
       phyaddr->channel_no, 
       phyaddr->chip_no,
       phyaddr->block_no,
       phyaddr->page_no);
       */

    if (logaddr->lpa_cg >= np->nr_pages_per_ssd) {
        bdbm_error ("LPA is beyond logical space (%llX)", logaddr->lpa[k]);
        return 1;
    }

    /* get the mapping entry for lpa */
    me = &p->ptr_mapping_table[logaddr->lpa_cg];
    bdbm_bug_on (me == NULL);

    for (k = 0; k < np->nr_subpages_per_page; k++) {
        if(logaddr->lpa[k] != -1){
            spme = find_lpa_4kb(p, logaddr->lpa[k]);
            if(spme != NULL){
                bdbm_abm_invalidate_page_4kb (
                        p->bai, 
                        spme->ppa.channel_no, 
                        spme->ppa.chip_no, 
                        spme->ppa.block_no, 
                        spme->ppa.page_no, 
                        spme->sp_off
                        );
                invalidate_lpa_4kb(p, logaddr->lpa[k]);
                printf("logaddr=%d, ofs=%d, phy:: ch=%d, chip=%d, block=%d, page_no=%d sub page = %d\n",
                        logaddr->lpa[logaddr->ofs], logaddr->ofs,
                        spme->ppa.channel_no, 
                        spme->ppa.chip_no, 
                        spme->ppa.block_no, 
                        spme->ppa.page_no, 
                        spme->sp_off);


            }
        }
    }

    /* update the mapping table */
    if (me->status == PFTL_PAGE_VALID) {
        for (k = 0; k < np->nr_subpages_per_page; k++) {
            bdbm_abm_invalidate_page (
                    p->bai, 
                    me->phyaddr.channel_no, 
                    me->phyaddr.chip_no,
                    me->phyaddr.block_no,
                    me->phyaddr.page_no,
                    k
                    );
        }
    }

    me->status = PFTL_PAGE_VALID;
    me->phyaddr.channel_no = phyaddr->channel_no;
    me->phyaddr.chip_no = phyaddr->chip_no;
    me->phyaddr.block_no = phyaddr->block_no;
    me->phyaddr.page_no = phyaddr->page_no;
    me->sp_off = 0;

    return 0;
}

uint32_t bdbm_fgm_ftl_map_lpa_to_ppa (
        bdbm_drv_info_t* bdi, 
        bdbm_logaddr_t* logaddr,
        bdbm_phyaddr_t* phyaddr)
{
    if(logaddr->lpa_cg == -1){
        return __bdbm_fgm_ftl_map_lpa_to_ppa_4kb(bdi, logaddr, phyaddr);
    }else{
        return __bdbm_fgm_ftl_map_lpa_to_ppa(bdi, logaddr, phyaddr);
    }
}

uint32_t __bdbm_fgm_ftl_get_ppa (
        bdbm_drv_info_t* bdi, 
        int64_t lpa,
        bdbm_phyaddr_t* phyaddr,
        uint64_t* sp_off)
{
    bdbm_device_params_t* np = BDBM_GET_DEVICE_PARAMS (bdi);
    bdbm_fgm_ftl_private_t* p = _ftl_fgm_ftl.ptr_private;
    bdbm_fgm_mapping_entry_t* me = NULL;
    uint32_t ret;

    /* get the mapping entry for lpa */
    me = &p->ptr_mapping_table[lpa];

    /* NOTE: sometimes a file system attempts to read 
     * a logical address that was not written before.
     * in that case, we return 'address 0' */
    if (me->status != PFTL_PAGE_VALID) {
        phyaddr->channel_no = 0;
        phyaddr->chip_no = 0;
        phyaddr->block_no = 0;
        phyaddr->page_no = 0;
        phyaddr->punit_id = 0;
        *sp_off = 0;
        ret = 1;
    } else {
        phyaddr->channel_no = me->phyaddr.channel_no;
        phyaddr->chip_no = me->phyaddr.chip_no;
        phyaddr->block_no = me->phyaddr.block_no;
        phyaddr->page_no = me->phyaddr.page_no;
        phyaddr->punit_id = BDBM_GET_PUNIT_ID (bdi, phyaddr);
        *sp_off = me->sp_off;
        ret = 0;
    }

    return ret;
}

uint32_t bdbm_fgm_ftl_get_ppa (
        bdbm_drv_info_t* bdi, 
        bdbm_logaddr_t* logaddr,
        bdbm_phyaddr_t* phyaddr,
        uint64_t* sp_off)
{
    bdbm_device_params_t* np = BDBM_GET_DEVICE_PARAMS (bdi);
    bdbm_fgm_ftl_private_t* p = _ftl_fgm_ftl.ptr_private;
    bdbm_fgm_mapping_entry_t* me = NULL;
    bdbm_fgm_sp_mapping_entry_t* spme = NULL;
    uint32_t ret, k, is_rmw_get_ppa = 0;

    for (k = 0; k < np->nr_subpages_per_page; k++) {
        if(logaddr->lpa[k] != -1){
            is_rmw_get_ppa++;

            if (logaddr->lpa[k] >= np->nr_subpages_per_ssd) {
                bdbm_error ("A given lpa is beyond logical space (%llu)", logaddr->lpa[k]);
                return 1;
            }

            spme = find_lpa_4kb(p, logaddr->lpa[k]);
            if(spme != NULL){
                phyaddr->channel_no = spme->ppa.channel_no; 
                phyaddr->chip_no = spme->ppa.chip_no;
                phyaddr->block_no = spme->ppa.block_no; 
                phyaddr->page_no = spme->ppa.page_no;
                *sp_off = spme->sp_off;
            }
        }
    }

    if(is_rmw_get_ppa == 1)
        return 0;

    logaddr->ofs = 0;
    if (logaddr->lpa_cg >= np->nr_pages_per_ssd) {
        bdbm_error ("A given lpa is beyond logical space (%llu)", logaddr->lpa_cg);
        return 1;
    }

    return __bdbm_fgm_ftl_get_ppa(bdi, logaddr->lpa_cg, phyaddr, sp_off);
}


uint32_t bdbm_fgm_ftl_invalidate_lpa (
        bdbm_drv_info_t* bdi, 
        int64_t lpa, 
        uint64_t len)
{	
    bdbm_device_params_t* np = BDBM_GET_DEVICE_PARAMS (bdi);
    bdbm_fgm_ftl_private_t* p = _ftl_fgm_ftl.ptr_private;
    bdbm_fgm_mapping_entry_t* me = NULL;
    uint64_t loop, k;

    /* check the range of input addresses */
    if ((lpa + len) > np->nr_pages_per_ssd) {
        bdbm_warning ("LPA is beyond logical space (%llu = %llu+%llu) %llu", 
                lpa+len, lpa, len, np->nr_pages_per_ssd);
        return 1;
    }

    /* make them invalid */
    for (loop = lpa; loop < (lpa + len); loop++) {
        me = &p->ptr_mapping_table[loop];
        if (me->status == PFTL_PAGE_VALID) {
            for (k = 0; k < np->nr_subpages_per_page; k++) {
                bdbm_abm_invalidate_page (
                        p->bai, 
                        me->phyaddr.channel_no, 
                        me->phyaddr.chip_no,
                        me->phyaddr.block_no,
                        me->phyaddr.page_no,
                        k
                        );
                me->status = PFTL_PAGE_INVALID;
            }
        }
    }

    return 0;
}

uint8_t bdbm_fgm_ftl_is_gc_needed (bdbm_drv_info_t* bdi, int64_t lpa)
{
    bdbm_fgm_ftl_private_t* p = _ftl_fgm_ftl.ptr_private;
    uint64_t nr_total_blks = bdbm_abm_get_nr_total_blocks (p->bai);
    uint64_t nr_free_blks = bdbm_abm_get_nr_free_blocks (p->bai);

    /* invoke gc when remaining free blocks are less than 1% of total blocks */
    if ((nr_free_blks * 100 / nr_total_blks) <= 2) {
        return 1;
    }

    /* invoke gc when there is only one dirty block (for debugging) */
    /*
       bdbm_fgm_ftl_private_t* p = _ftl_fgm_ftl.ptr_private;
       if (bdbm_abm_get_nr_dirty_blocks (p->bai) > 1) {
       return 1;
       }
       */

    return 0;
}

/* VICTIM SELECTION - First Selection:
 * select the first dirty block in a list */
bdbm_abm_block_t* __bdbm_fgm_ftl_victim_selection (
        bdbm_drv_info_t* bdi,
        uint64_t channel_no,
        uint64_t chip_no)
{
    bdbm_fgm_ftl_private_t* p = _ftl_fgm_ftl.ptr_private;
    bdbm_device_params_t* np = BDBM_GET_DEVICE_PARAMS (bdi);
    bdbm_abm_block_t* a = NULL;
    bdbm_abm_block_t* b = NULL;
    struct list_head* pos = NULL;

    a = p->ac_bab[channel_no*np->nr_chips_per_channel + chip_no];
    bdbm_abm_list_for_each_dirty_block (pos, p->bai, channel_no, chip_no) {
        b = bdbm_abm_fetch_dirty_block (pos);
        if (a != b)
            break;
        b = NULL;
    }

    return b;
}

/* VICTIM SELECTION - Greedy:
 * select a dirty block with a small number of valid pages */
bdbm_abm_block_t* __bdbm_fgm_ftl_victim_selection_greedy (
        bdbm_drv_info_t* bdi,
        uint64_t channel_no,
        uint64_t chip_no)
{
    bdbm_fgm_ftl_private_t* p = _ftl_fgm_ftl.ptr_private;
    bdbm_device_params_t* np = BDBM_GET_DEVICE_PARAMS (bdi);
    bdbm_abm_block_t* a = NULL;
    bdbm_abm_block_t* b = NULL;
    bdbm_abm_block_t* v = NULL;
    struct list_head* pos = NULL;

    a = p->ac_bab[channel_no*np->nr_chips_per_channel + chip_no];

    bdbm_abm_list_for_each_dirty_block (pos, p->bai, channel_no, chip_no) {
        b = bdbm_abm_fetch_dirty_block (pos);
        if (a == b)
            continue;
        if (b->nr_invalid_subpages == np->nr_subpages_per_block) {
            v = b;
            break;
        }
        if (v == NULL) {
            v = b;
            continue;
        }
        if (b->nr_invalid_subpages > v->nr_invalid_subpages)
            v = b;
    }

    return v;
}

uint32_t bdbm_fgm_ftl_do_gc (bdbm_drv_info_t* bdi, int64_t lpa)
{
    bdbm_fgm_ftl_private_t* p = _ftl_fgm_ftl.ptr_private;
    bdbm_device_params_t* np = BDBM_GET_DEVICE_PARAMS (bdi);
    bdbm_hlm_req_gc_t* hlm_gc = &p->gc_hlm;
    bdbm_hlm_req_gc_t* hlm_gc_w = &p->gc_hlm_w;
    uint64_t nr_gc_blks = 0;
    uint64_t nr_llm_reqs = 0;
    uint64_t nr_punits = 0;
    uint64_t i, j, k;
    bdbm_stopwatch_t sw;

    printf("GC: START\n");
    nr_punits = np->nr_channels * np->nr_chips_per_channel;

    /* choose victim blocks for individual parallel units */
    bdbm_memset (p->gc_bab, 0x00, sizeof (bdbm_abm_block_t*) * nr_punits);
    bdbm_stopwatch_start (&sw);
    for (i = 0, nr_gc_blks = 0; i < np->nr_channels; i++) {
        for (j = 0; j < np->nr_chips_per_channel; j++) {
            bdbm_abm_block_t* b; 
            // printf("chan=%d, chip=%d, block=%d\n", b->channel_no, b->chip_no,
            //                 b->block_no);

            if ((b = __bdbm_fgm_ftl_victim_selection_greedy (bdi, i, j))) {
                p->gc_bab[nr_gc_blks] = b;
                nr_gc_blks++;
            }
        }
    }
    if (nr_gc_blks < nr_punits) {
        /* TODO: we need to implement a load balancing feature to avoid this */
        /*bdbm_warning ("TODO: this warning will be removed with load-balancing");*/
        return 0;
    }

    /* TEMP */
    for (i = 0; i < nr_punits * np->nr_pages_per_block; i++) {
        hlm_reqs_pool_reset_fmain (&hlm_gc->llm_reqs[i].fmain);
    }
    /* TEMP */

    /* build hlm_req_gc for reads */
    for (i = 0, nr_llm_reqs = 0; i < nr_gc_blks; i++) {
        bdbm_abm_block_t* b = p->gc_bab[i];
        if (b == NULL)
            break;
        for (j = 0; j < np->nr_pages_per_block; j++) {
            bdbm_llm_req_t* r = &hlm_gc->llm_reqs[nr_llm_reqs];
            int has_valid = 0;
            /* are there any valid subpages in a block */
            hlm_reqs_pool_reset_fmain (&r->fmain);
            hlm_reqs_pool_reset_logaddr (&r->logaddr);
            for (k = 0; k < np->nr_subpages_per_page; k++) {
                if (b->pst[j*np->nr_subpages_per_page+k] != BDBM_ABM_SUBPAGE_INVALID) {
                    has_valid = 1;
                    r->logaddr.lpa[k] = -1; /* the subpage contains new data */
                    r->fmain.kp_stt[k] = KP_STT_DATA;
                } else {
                    r->logaddr.lpa[k] = -1;	/* the subpage contains obsolate data */
                    r->fmain.kp_stt[k] = KP_STT_HOLE;
                }
            }
            /* if it is, selects it as the gc candidates */
            if (has_valid) {
                r->req_type = REQTYPE_GC_READ;
                r->phyaddr.channel_no = b->channel_no;
                r->phyaddr.chip_no = b->chip_no;
                r->phyaddr.block_no = b->block_no;
                r->phyaddr.page_no = j;
                r->phyaddr.punit_id = BDBM_GET_PUNIT_ID (bdi, (&r->phyaddr));
                r->ptr_hlm_req = (void*)hlm_gc;
                r->ret = 0;
                nr_llm_reqs++;
            }
        }
    }

    /*
       bdbm_msg ("----------------------------------------------");
       bdbm_msg ("gc-victim: %llu pages, %llu blocks, %llu us", 
       nr_llm_reqs, nr_gc_blks, bdbm_stopwatch_get_elapsed_time_us (&sw));
       */

    /* wait until Q in llm becomes empty 
     * TODO: it might be possible to further optimize this */
    bdi->ptr_llm_inf->flush (bdi);

    if (nr_llm_reqs == 0) 
        goto erase_blks;

    /* send read reqs to llm */
    hlm_gc->req_type = REQTYPE_GC_READ;
    hlm_gc->nr_llm_reqs = nr_llm_reqs;
    atomic64_set (&hlm_gc->nr_llm_reqs_done, 0);
    bdbm_sema_lock (&hlm_gc->done);
    for (i = 0; i < nr_llm_reqs; i++) {
        if ((bdi->ptr_llm_inf->make_req (bdi, &hlm_gc->llm_reqs[i])) != 0) {
            bdbm_error ("llm_make_req failed");
            bdbm_bug_on (1);
        }
    }
    bdbm_sema_lock (&hlm_gc->done);
    bdbm_sema_unlock (&hlm_gc->done);


    /* perform write compaction for gc */
    /*bdbm_track ();*/
    hlm_reqs_pool_write_compaction (hlm_gc_w, hlm_gc, np);

    /*bdbm_msg ("compaction: %llu => %llu", nr_llm_reqs, hlm_gc_w->nr_llm_reqs);*/

    nr_llm_reqs = hlm_gc_w->nr_llm_reqs;

    /* build hlm_req_gc for writes */
    for (i = 0; i < nr_llm_reqs; i++) {
        bdbm_llm_req_t* r = &hlm_gc_w->llm_reqs[i];
        r->req_type = REQTYPE_GC_WRITE;	/* change to write */
        for (k = 0; k < np->nr_subpages_per_page; k++) {
            /* move subpages that contain new data */
            if (r->fmain.kp_stt[k] == KP_STT_DATA) {
                r->logaddr.lpa[k] = ((uint64_t*)r->foob.data)[k];
            } else if (r->fmain.kp_stt[k] == KP_STT_HOLE) {
                ((uint64_t*)r->foob.data)[k] = -1;
                r->logaddr.lpa[k] = -1;
            } else {
                bdbm_bug_on (1);
            }
        }
        r->ptr_hlm_req = (void*)hlm_gc_w;
        if (bdbm_fgm_ftl_get_free_ppa (bdi, 0, &r->phyaddr) != 0) {
            bdbm_error ("bdbm_fgm_ftl_get_free_ppa failed");
            bdbm_bug_on (1);
        }
        if (bdbm_fgm_ftl_map_lpa_to_ppa (bdi, &r->logaddr, &r->phyaddr) != 0) {
            bdbm_error ("bdbm_fgm_ftl_map_lpa_to_ppa failed");
            bdbm_bug_on (1);
        }
    }

    /* send write reqs to llm */
    hlm_gc_w->req_type = REQTYPE_GC_WRITE;
    hlm_gc_w->nr_llm_reqs = nr_llm_reqs;
    atomic64_set (&hlm_gc_w->nr_llm_reqs_done, 0);
    bdbm_sema_lock (&hlm_gc_w->done);
    for (i = 0; i < nr_llm_reqs; i++) {
        if ((bdi->ptr_llm_inf->make_req (bdi, &hlm_gc_w->llm_reqs[i])) != 0) {
            bdbm_error ("llm_make_req failed");
            bdbm_bug_on (1);
        }
    }
    bdbm_sema_lock (&hlm_gc_w->done);
    bdbm_sema_unlock (&hlm_gc_w->done);

    /* erase blocks */
erase_blks:
    for (i = 0; i < nr_gc_blks; i++) {
        bdbm_abm_block_t* b = p->gc_bab[i];
        bdbm_llm_req_t* r = &hlm_gc->llm_reqs[i];
        r->req_type = REQTYPE_GC_ERASE;
        r->logaddr.lpa[0] = -1ULL; /* lpa is not available now */
        r->phyaddr.channel_no = b->channel_no;
        r->phyaddr.chip_no = b->chip_no;
        r->phyaddr.block_no = b->block_no;
        r->phyaddr.page_no = 0;
        r->phyaddr.punit_id = BDBM_GET_PUNIT_ID (bdi, (&r->phyaddr));
        r->ptr_hlm_req = (void*)hlm_gc;
        r->ret = 0;
    }

    /* send erase reqs to llm */
    hlm_gc->req_type = REQTYPE_GC_ERASE;
    hlm_gc->nr_llm_reqs = p->nr_punits;
    atomic64_set (&hlm_gc->nr_llm_reqs_done, 0);
    bdbm_sema_lock (&hlm_gc->done);
    for (i = 0; i < nr_gc_blks; i++) {
        if ((bdi->ptr_llm_inf->make_req (bdi, &hlm_gc->llm_reqs[i])) != 0) {
            bdbm_error ("llm_make_req failed");
            bdbm_bug_on (1);
        }
    }
    bdbm_sema_lock (&hlm_gc->done);
    bdbm_sema_unlock (&hlm_gc->done);

    /* FIXME: what happens if block erasure fails */
    for (i = 0; i < nr_gc_blks; i++) {
        uint8_t ret = 0;
        bdbm_abm_block_t* b = p->gc_bab[i];
        if (hlm_gc->llm_reqs[i].ret != 0) 
            ret = 1;	/* bad block */
        bdbm_abm_erase_block (p->bai, b->channel_no, b->chip_no, b->block_no, ret);
    }

    printf("GC: END\n");
    return 0;
}

void __bdbm_fgm_badblock_scan_eraseblks (
        bdbm_drv_info_t* bdi,
        uint64_t block_no)
{
    bdbm_fgm_ftl_private_t* p = _ftl_fgm_ftl.ptr_private;
    bdbm_device_params_t* np = BDBM_GET_DEVICE_PARAMS (bdi);
    bdbm_hlm_req_gc_t* hlm_gc = &p->gc_hlm;
    uint64_t i, j;

    /* setup blocks to erase */
    bdbm_memset (p->gc_bab, 0x00, sizeof (bdbm_abm_block_t*) * p->nr_punits);
    for (i = 0; i < np->nr_channels; i++) {
        for (j = 0; j < np->nr_chips_per_channel; j++) {
            bdbm_abm_block_t* b = NULL;
            bdbm_llm_req_t* r = NULL;
            uint64_t punit_id = i*np->nr_chips_per_channel+j;

            if ((b = bdbm_abm_get_block (p->bai, i, j, block_no)) == NULL) {
                bdbm_error ("oops! bdbm_abm_get_block failed");
                bdbm_bug_on (1);
            }
            p->gc_bab[punit_id] = b;

            r = &hlm_gc->llm_reqs[punit_id];
            r->req_type = REQTYPE_GC_ERASE;
            r->logaddr.lpa[0] = -1ULL; /* lpa is not available now */
            r->phyaddr.channel_no = b->channel_no;
            r->phyaddr.chip_no = b->chip_no;
            r->phyaddr.block_no = b->block_no;
            r->phyaddr.page_no = 0;
            r->phyaddr.punit_id = BDBM_GET_PUNIT_ID (bdi, (&r->phyaddr));
            r->ptr_hlm_req = (void*)hlm_gc;
            r->ret = 0;
        }
    }

    /* send erase reqs to llm */
    hlm_gc->req_type = REQTYPE_GC_ERASE;
    hlm_gc->nr_llm_reqs = p->nr_punits;
    atomic64_set (&hlm_gc->nr_llm_reqs_done, 0);
    bdbm_sema_lock (&hlm_gc->done);
    for (i = 0; i < p->nr_punits; i++) {
        if ((bdi->ptr_llm_inf->make_req (bdi, &hlm_gc->llm_reqs[i])) != 0) {
            bdbm_error ("llm_make_req failed");
            bdbm_bug_on (1);
        }
    }
    bdbm_sema_lock (&hlm_gc->done);
    bdbm_sema_unlock (&hlm_gc->done);

    for (i = 0; i < p->nr_punits; i++) {
        uint8_t ret = 0;
        bdbm_abm_block_t* b = p->gc_bab[i];

        if (hlm_gc->llm_reqs[i].ret != 0) {
            ret = 1; /* bad block */
        }

        bdbm_abm_erase_block (p->bai, b->channel_no, b->chip_no, b->block_no, ret);
    }

    /* measure gc elapsed time */
}

static void __bdbm_fgm_mark_it_dead (
        bdbm_drv_info_t* bdi,
        uint64_t block_no)
{
    bdbm_fgm_ftl_private_t* p = _ftl_fgm_ftl.ptr_private;
    bdbm_device_params_t* np = BDBM_GET_DEVICE_PARAMS (bdi);
    int i, j;

    for (i = 0; i < np->nr_channels; i++) {
        for (j = 0; j < np->nr_chips_per_channel; j++) {
            bdbm_abm_block_t* b = NULL;

            if ((b = bdbm_abm_get_block (p->bai, i, j, block_no)) == NULL) {
                bdbm_error ("oops! bdbm_abm_get_block failed");
                bdbm_bug_on (1);
            }

            bdbm_abm_set_to_dirty_block (p->bai, i, j, block_no);
        }
    }
}

uint32_t bdbm_fgm_badblock_scan (bdbm_drv_info_t* bdi)
{
    bdbm_fgm_ftl_private_t* p = _ftl_fgm_ftl.ptr_private;
    bdbm_device_params_t* np = BDBM_GET_DEVICE_PARAMS (bdi);
    bdbm_fgm_mapping_entry_t* me = NULL;
    uint64_t i = 0;
    uint32_t ret = 0;

    bdbm_msg ("[WARNING] 'bdbm_fgm_badblock_scan' is called! All of the flash blocks will be erased!!!");

    /* step1: reset the page-level mapping table */
    bdbm_msg ("step1: reset the page-level mapping table");
    me = p->ptr_mapping_table;
    for (i = 0; i < np->nr_subpages_per_ssd; i++) {
        me[i].status = PFTL_PAGE_NOT_ALLOCATED;
        me[i].phyaddr.channel_no = PFTL_PAGE_INVALID_ADDR;
        me[i].phyaddr.chip_no = PFTL_PAGE_INVALID_ADDR;
        me[i].phyaddr.block_no = PFTL_PAGE_INVALID_ADDR;
        me[i].phyaddr.page_no = PFTL_PAGE_INVALID_ADDR;
        me[i].sp_off = -1;
    }

    /* step2: erase all the blocks */
    bdi->ptr_llm_inf->flush (bdi);
    for (i = 0; i < np->nr_blocks_per_chip; i++) {
        __bdbm_fgm_badblock_scan_eraseblks (bdi, i);
    }

    /* step3: store abm */
    if ((ret = bdbm_abm_store (p->bai, "/usr/share/bdbm_drv/abm.dat"))) {
        bdbm_error ("bdbm_abm_store failed");
        return 1;
    }

    /* step4: get active blocks */
    bdbm_msg ("step2: get active blocks");
    if (__bdbm_fgm_ftl_get_active_blocks (np, p->bai, p->ac_bab) != 0) {
        bdbm_error ("__bdbm_fgm_ftl_get_active_blocks failed");
        return 1;
    }
    p->curr_puid = 0;
    p->curr_page_ofs = 0;

    bdbm_msg ("done");

    return 0;

}