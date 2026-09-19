#include "items.h"
#include "core/bitter_assert.h"
#include "core/bitter_string.h"

int bitter_key_cmp(const struct bitter_key_cpu *a, const struct bitter_key_cpu *b) {
  if (a->objectid < b->objectid) {
    return -1;
  }
  else if (a->objectid > b->objectid) {
    return 1;
  }
  if (a->type < b->type) {
    return -1;
  }
  else if (a->type > b->type) {
    return 1;
  }
  if (a->offset < b->offset) {
    return -1;
  }
  else if (a->offset > b->offset) {
    return 1;
  }
  return 0;
  
}

int bitter_key_cmp_disk(const struct bitter_key *disk, const struct bitter_key_cpu *b) {
  
  bt_u64 disk_objectid = bt_get_le64(&disk->objectid);
  bt_u8 disk_type = disk->type;
  bt_u64 disk_offset = bt_get_le64(&disk->offset);
  
  if (disk_objectid < b->objectid) {
    return -1;
  }
  else if (disk_objectid > b->objectid) {
    return 1;
  }
  if (disk_type < b->type) {
    return -1;
  }
  else if (disk_type > b->type) {
    return 1;
  }
  if (disk_offset < b->offset) {
    return -1;
  }
  else if (disk_offset > b->offset) {
    return 1;
  }
  return 0;

  
} 

void bitter_key_from_disk(struct bitter_key_cpu *dst, const struct bitter_key *src) {
  dst->objectid = bt_get_le64(&src->objectid);
  dst->type = src->type;
  dst->offset = bt_get_le64(&src->offset); 
}


void bitter_key_to_disk(struct bitter_key *dst, const struct bitter_key_cpu *src) {
  bt_put_le64(&dst->objectid, src->objectid);
  dst->type = src->type;
  bt_put_le64(&dst->offset, src->offset);
}

bt_u32 bitter_leaf_nritems(const void* block) {
  const struct bitter_header* hdr = block;
  return bt_get_le32(&hdr->nritems);
}

void bitter_leaf_set_nritems(void* block, bt_u32 items) {
  struct bitter_header* hdr = block;
  bt_put_le32(&hdr->nritems, items);
}


struct bitter_item* bitter_leaf_item(void* block, bt_u32 slot) {
  
  bt_u8* hdr = (bt_u8*)block;
  hdr += slot * sizeof(struct bitter_item) + BITTER_HEADER_SIZE;
  return (struct bitter_item*)hdr;

}

void* bitter_leaf_data(void *block, bt_u32 slot) {
  
  struct bitter_item* item = bitter_leaf_item(block, slot);
  bt_u32 offset = bt_get_le32(&item->offset);
  bt_u8* hdr = (bt_u8*)block;
  hdr += offset + BITTER_HEADER_SIZE;
  return hdr;
}

bt_u32 bitter_leaf_free_space(void* block) {
  
  bt_u32 nritems = bitter_leaf_nritems(block);

  if (nritems == 0) {
    return BITTER_LEAF_DATA_SIZE;
  }
 
  struct bitter_item* last_item = bitter_leaf_item(block, nritems - 1);
  
  bt_u32 offset = bt_get_le32(&last_item->offset);

  return offset - sizeof(struct bitter_item) * nritems; 

}

bt_u32 bitter_leaf_range_size(void* block, bt_u32 from, bt_u32 to) {

  BITTER_ASSERT(from <= to);
  BITTER_ASSERT(to <= bitter_leaf_nritems(block));
 
  if (from == to) {
    return 0;
  }
 
  bt_u32 from_size = (from == 0) ? BITTER_LEAF_DATA_SIZE : 
            bt_get_le32(&bitter_leaf_item(block, from - 1)->offset);
  bt_u32 to_size = bt_get_le32(&bitter_leaf_item(block, to - 1)->offset);
  bt_u32 payload = from_size - to_size;
  /* sizeof drags the expression to 64 bits; the result is bounded by
   * BITTER_LEAF_DATA_SIZE, so the narrowing is safe and explicit. */
  return (bt_u32)(payload + sizeof(struct bitter_item) * (to - from));

}









/*
 * Puts an item at `slot`, shifting everything at or after it out of the way.
 *
 * Cannot fail: the caller guarantees room, because deciding to split a full
 * leaf is btree.c's job.  Both assertions are about the CALLER being correct,
 * which is why they abort rather than return -- see core/bitter_assert.h.
 *
 * Every offset and length is read BEFORE the first memmove.  After the
 * descriptors shift, item[i] no longer means what it did, and reading through
 * a stale index is the easiest way to get this wrong.
 */
void bitter_leaf_insert(void* block, const struct bitter_key_cpu *key, bt_u32 slot,
     const void* data, bt_u32 size) {

  const bt_u32 ITEM = (bt_u32)sizeof(struct bitter_item);
  bt_u32 nritems = bitter_leaf_nritems(block);

  /* slot == nritems is LEGAL: it means append.  Inserting the very first item
   * into an empty leaf is slot 0 with nritems 0. */
  BITTER_ASSERT(slot <= nritems);
  /* Room for the payload AND the new descriptor -- forgetting the second term
   * is the classic off-by-25. */
  BITTER_ASSERT(bitter_leaf_free_space(block) >= size + ITEM);

  bt_u32 items_ahead = nritems - slot;

  /* Where the new payload goes: immediately below slot-1's, or flush with the
   * end of the data area when inserting at slot 0.  There is no item[-1], so
   * this cannot be a plain bitter_leaf_item(block, slot - 1). */
  bt_u32 prev_offset = (slot == 0)
      ? BITTER_LEAF_DATA_SIZE
      : bitter_item_get_offset(bitter_leaf_item(block, slot - 1));

  /* The payload region that has to move down: everything belonging to slots
   * slot..nritems-1.  Its lowest byte is item[nritems-1].offset and it extends
   * up to prev_offset -- note that is the offset of the item ABOVE the group,
   * not of item[slot], whose own payload is part of what moves. */
  bt_u32 move_len = 0;
  bt_u8* move_src = 0;
  if (items_ahead > 0) {
    bt_u32 lowest = bitter_item_get_offset(bitter_leaf_item(block, nritems - 1));
    move_len = prev_offset - lowest;
    move_src = (bt_u8*)block + BITTER_HEADER_SIZE + lowest;
  }

  /* 1. Descriptors [slot, nritems) forward by one item's width. */
  bt_u8* slot_loc = (bt_u8*)bitter_leaf_item(block, slot);
  memmove(slot_loc + ITEM, slot_loc, items_ahead * ITEM);

  /* 2. Their payloads down by `size`.  memmove, not memcpy: the regions
   * overlap whenever move_len > size. */
  if (move_len > 0) {
    memmove(move_src - size, move_src, move_len);
  }

  /* 3. Rewrite the offsets of everything that just moved.  THE STEP THAT GETS
   * SKIPPED: those descriptors were relocated in step 1, but their offset
   * fields still point at where their payloads used to be.  After step 1 they
   * live at slot+1 .. slot+items_ahead. */
  for (bt_u32 i = 1; i <= items_ahead; i++) {
    struct bitter_item* item = bitter_leaf_item(block, slot + i);
    bitter_item_set_offset(item, bitter_item_get_offset(item) - size);
  }

  /* 4. The new descriptor.  The key is part of it -- an item with the right
   * payload and no key is invisible to every search. */
  struct bitter_item* new_item = bitter_leaf_item(block, slot);
  bitter_key_to_disk(&new_item->key, key);
  bitter_item_set_offset(new_item, prev_offset - size);
  bitter_item_set_size(new_item, size);

  /* 5. memcpy, not memmove: `data` is the caller's buffer and cannot overlap
   * the block. */
  memcpy(bitter_leaf_data(block, slot), data, size);

  /* 6. Last, so the leaf is only ever briefly inconsistent. */
  bitter_leaf_set_nritems(block, nritems + 1);
}

/*
 * Removes the item at `slot`, closing the gap.  The reverse of insert, and
 * easier because nothing can fail to fit.
 *
 * The ordering trap is different here: the victim's size and offset must be
 * read BEFORE the descriptor shift, which overwrites its descriptor.
 */
void bitter_leaf_remove(void* block, bt_u32 slot) {

  const bt_u32 ITEM = (bt_u32)sizeof(struct bitter_item);
  bt_u32 nritems = bitter_leaf_nritems(block);

  /* Strictly less than: remove needs an item that exists, where insert accepts
   * slot == nritems.  Using the same bound in both would break one of them. */
  BITTER_ASSERT(slot < nritems);

  bt_u32 items_ahead = nritems - slot - 1;

  /* Read the victim first -- step 4 destroys this descriptor. */
  struct bitter_item* victim = bitter_leaf_item(block, slot);
  bt_u32 size          = bitter_item_get_size(victim);
  bt_u32 victim_offset = bitter_item_get_offset(victim);

  /* The payloads below the victim's, which move UP into the space it leaves. */
  bt_u32 move_len = 0;
  bt_u8* move_src = 0;
  if (items_ahead > 0) {
    bt_u32 lowest = bitter_item_get_offset(bitter_leaf_item(block, nritems - 1));
    move_len = victim_offset - lowest;
    move_src = (bt_u8*)block + BITTER_HEADER_SIZE + lowest;
  }

  if (move_len > 0) {
    memmove(move_src + size, move_src, move_len);
  }

  /* Descriptors after `slot` shift down over it. */
  bt_u8* slot_loc = (bt_u8*)bitter_leaf_item(block, slot);
  memmove(slot_loc, slot_loc + ITEM, items_ahead * ITEM);

  /* Rewrite the offsets of what moved.  Note the index range differs from
   * insert's: after shifting DOWN, those descriptors sit at
   * slot .. slot+items_ahead-1, not slot+1 onward. */
  for (bt_u32 i = 0; i < items_ahead; i++) {
    struct bitter_item* item = bitter_leaf_item(block, slot + i);
    bitter_item_set_offset(item, bitter_item_get_offset(item) + size);
  }

  bitter_leaf_set_nritems(block, nritems - 1);

  /* Not required -- nothing reads vacated bytes -- but it makes two leaves
   * with the same logical contents byte-identical, which is what lets a
   * golden-image test diff them.
   *
   * The gap the memmove opened starts at `lowest`, which is victim_offset -
   * move_len.  Stated that way rather than reusing `lowest` because it also
   * covers items_ahead == 0, where move_len is 0 and the vacated bytes are the
   * victim's own payload at victim_offset. */
  memset((bt_u8*)block + BITTER_HEADER_SIZE + victim_offset - move_len, 0, size);
  memset(slot_loc + items_ahead * ITEM, 0, ITEM);
}


/*
 * The five things that must be true of every leaf.  See the layout diagram in
 * items.h; each check below corresponds to one property of it.
 *
 * Deliberately independent of bitter_leaf_free_space: if both computed the
 * free space the same way, a shared mistake would agree with itself.
 */
bt_u32 bitter_leaf_check(void* block) {
  bt_u32 flags = 0;
  bt_u32 nritems = bitter_leaf_nritems(block);
  bt_u32 item_sz = (bt_u32)sizeof(struct bitter_item);
  bt_u32 max_items = BITTER_LEAF_DATA_SIZE / item_sz;

  /* First, because every later check indexes descriptors: a wild nritems
   * would walk off the block before anything else could complain. */
  if (nritems > max_items) {
    return BITTER_LEAF_ERR_NRITEMS;
  }
  if (nritems == 0) {
    return 0;
  }

  bt_u32 desc_end = nritems * item_sz;   /* descriptors occupy [0, desc_end) */
  bt_u32 prev_offset = 0;
  struct bitter_key_cpu prev_key;

  for (bt_u32 i = 0; i < nritems; i++) {
    struct bitter_item* it = bitter_leaf_item(block, i);
    bt_u32 offset = bt_get_le32(&it->offset);
    bt_u32 size   = bt_get_le32(&it->size);

    /* Inside the data area at all.  offset + size is computed in 32 bits and
     * could wrap on a corrupt block, so compare the parts separately. */
    if (offset > BITTER_LEAF_DATA_SIZE ||
        size   > BITTER_LEAF_DATA_SIZE ||
        offset + size > BITTER_LEAF_DATA_SIZE) {
      flags |= BITTER_LEAF_ERR_BOUNDS;
      break;                 /* the remaining checks would read nonsense */
    }

    /* Payloads must not reach back into the descriptor array. */
    if (offset < desc_end) {
      flags |= BITTER_LEAF_ERR_OVERLAP;
    }

    if (i == 0) {
      /* Slot 0's payload is the highest, flush with the end of the block --
       * this is what makes the free space one contiguous run. */
      if (offset + size != BITTER_LEAF_DATA_SIZE) {
        flags |= BITTER_LEAF_ERR_ANCHOR;
      }
    } else {
      /* Each payload sits immediately below its predecessor's. */
      if (offset + size != prev_offset) {
        flags |= BITTER_LEAF_ERR_GAP;
      }
      /* Keys strictly ascending: equal keys are a bug, not a tie, since the
       * keyspace is meant to be unique within a tree. */
      struct bitter_key_cpu key;
      bitter_key_from_disk(&key, &it->key);
      if (bitter_key_cmp(&prev_key, &key) >= 0) {
        flags |= BITTER_LEAF_ERR_ORDER;
      }
    }

    bitter_key_from_disk(&prev_key, &it->key);
    prev_offset = offset;
  }

  return flags;
}



/*********************************************************************
SEPARATE PART OF THE SAME FILE:
The functions above are concerned with the manipulation of leaf blocks
the functions below are concerned with the manipulation of non-leaf blocks
********************************************************************/


struct bitter_key_ptr* bitter_node_key_ptr(void* block, bt_u32 slot) {

  bt_u8* key_ptr = block;
  key_ptr += slot * sizeof(struct bitter_key_ptr) + BITTER_HEADER_SIZE;
  return (struct bitter_key_ptr*)key_ptr;

}

bt_u64 bitter_node_blockptr(void* block, bt_u32 slot) {
  
  struct bitter_key_ptr* key_ptr = bitter_node_key_ptr(block, slot);
  return bt_get_le64(&key_ptr->blockptr);

}

bt_u64 bitter_node_generation(void* block, bt_u32 slot) {
  
  struct bitter_key_ptr* key_ptr = bitter_node_key_ptr(block, slot);
  return bt_get_le64(&key_ptr->generation);

}

bt_u32 bitter_node_free_slots(void *block)
{
        return (bt_u32)(BITTER_MAX_CHILDREN - bitter_leaf_nritems(block));
}



/*
 * Three invariants, against bitter_leaf_check's six.  A node stores fixed-size
 * entries with no payloads, so there is no anchor, no contiguity chain and
 * nothing that can run past the end of the data area.
 *
 * Returns rather than asserts, for the same reason as the leaf checker: this
 * validates data that came off a disk, and a corrupt block is an expected
 * condition rather than a bug in the caller.
 */
bt_u32 bitter_node_check(void* block) {
  bt_u32 flags = 0;
  bt_u32 nritems = bitter_leaf_nritems(block);

  /* First, because everything below indexes entries. */
  if (nritems > BITTER_MAX_CHILDREN) {
    return BITTER_NODE_ERR_NRITEMS;
  }
  /* Unlike a leaf, which is legitimately empty when freshly created, a node
   * exists only to point at children -- one with none cannot be descended and
   * would strand everything beneath it. */
  if (nritems == 0) {
    return BITTER_NODE_ERR_EMPTY;
  }

  struct bitter_key_cpu prev_key;

  for (bt_u32 i = 0; i < nritems; i++) {
    struct bitter_key_ptr* kp = bitter_node_key_ptr(block, i);
    bt_u64 blockptr = bt_get_le64(&kp->blockptr);

    /* Zero is never a valid block address -- the first BITTER_SUPER_OFFSET
     * bytes are reserved -- and every tree block is block-aligned.  This is
     * what stops a corrupt pointer becoming a read at an arbitrary offset,
     * which read_block would then report as a confusing short read. */
    if (blockptr == 0 || blockptr % BITTER_BLOCK_SIZE != 0) {
      flags |= BITTER_NODE_ERR_BLOCKPTR;
    }

    if (i > 0) {
      struct bitter_key_cpu key;
      bitter_key_from_disk(&key, &kp->key);
      /* Strictly ascending: two children claiming the same starting key means
       * a search cannot decide which to descend into. */
      if (bitter_key_cmp(&prev_key, &key) >= 0) {
        flags |= BITTER_NODE_ERR_ORDER;
      }
    }
    bitter_key_from_disk(&prev_key, &kp->key);
  }

  return flags;
}


/*
 * Binary search over n entries of `stride` bytes each, starting at `base`.
 *
 * Serves BOTH leaves and nodes because bitter_item and bitter_key_ptr each
 * begin with a struct bitter_key, so entry i's key is always at
 * base + i * stride.
 *
 * Returns the slot of the match, or -- when absent -- the INSERTION POINT: the
 * first entry whose key is greater, which may legitimately be n.  That is why
 * bitter_leaf_insert accepts slot == nritems, and it is what lets one call
 * serve lookup and insert alike.
 *
 * The [lo, hi) form is deliberate.  With hi = n and `while (lo < hi)`:
 *   - n == 0 needs no special case, the loop simply does not run;
 *   - hi = mid never underflows, unlike hi = mid - 1 at mid == 0;
 *   - lo == hi at exit, and that value is the answer.
 * The alternative (hi = n - 1, lo <= hi) underflows in two places on unsigned
 * types and needs the empty case handled separately.
 */
bt_u32 bitter_bsearch(const void* base, bt_u32 stride, bt_u32 n,
      const struct bitter_key_cpu* key, int* found) {

  /* Set once, before any branch, so no exit can leave it unwritten. */
  *found = 0;

  bt_u32 lo = 0;
  bt_u32 hi = n;

  while (lo < hi) {
    /* lo + (hi - lo)/2, not (lo + hi)/2: the latter can overflow.  Harmless at
     * n <= 160, but it costs nothing to write the form that never does.
     * mid < hi always, so `hi = mid` below strictly shrinks the interval. */
    bt_u32 mid = lo + (hi - lo) / 2;
    const struct bitter_key* disk_key =
        (const struct bitter_key*)((const bt_u8*)base + mid * stride);

    /* cmp is the DISK key relative to the search key. */
    int cmp = bitter_key_cmp_disk(disk_key, key);

    if (cmp < 0) {
      lo = mid + 1;      /* entry[mid] is smaller: the answer is to its right */
    } else if (cmp > 0) {
      hi = mid;          /* entry[mid] is larger: the answer is at or left of it */
    } else {
      *found = 1;
      return mid;
    }
  }

  /* No match.  lo == hi, and it is the first slot whose key exceeds the search
   * key -- possibly n, meaning the key sorts after everything here.  No
   * dereference of entry[lo]: at lo == n there is no such entry. */
  return lo;
}


void bitter_node_insert(void* block, bt_u32 slot, const struct bitter_key_cpu *key,
    bt_u64 blockptr, bt_u64 generation) {

  bt_u32 nritems = bitter_leaf_nritems(block);
  BITTER_ASSERT(slot <= nritems);
  bt_u32 items_after = nritems - slot;
  BITTER_ASSERT(bitter_node_free_slots(block) >= 1);
  bt_u8* slot_loc = (bt_u8*)bitter_node_key_ptr(block, slot);
  memmove(slot_loc + sizeof(struct bitter_key_ptr), slot_loc, 
          sizeof(struct bitter_key_ptr)* items_after);    
  struct bitter_key_ptr* ptr = (struct bitter_key_ptr*)slot_loc;
  bt_put_le64(&ptr->blockptr, blockptr);
  bt_put_le64(&ptr->generation, generation);
  bitter_key_to_disk(&ptr->key, key);
  struct bitter_header* hdr = (struct bitter_header*) block;
  bt_put_le32(&hdr->nritems, nritems+ 1);
}

bt_u32 bitter_leaf_split_point(void* block) {

  bt_u32 half_space_used = BITTER_LEAF_DATA_SIZE - bitter_leaf_free_space(block);
  half_space_used /= 2;
  struct bitter_item* item = (struct bitter_item*)((bt_u8*)block + BITTER_HEADER_SIZE);
  bt_u32 n = 0; 
  for (bt_u32 size_seen = 0; size_seen < half_space_used;) {
    size_seen += sizeof(struct bitter_item) + bt_get_le32(&item->size);
    item = (struct bitter_item*)((bt_u8*)item + sizeof(struct bitter_item));
    n++;
  }
  if (n == 0) {
    return n+1;
  }
  if (n == bitter_leaf_nritems(block)) {
    return n-1;
  }
  
  return n;

}

