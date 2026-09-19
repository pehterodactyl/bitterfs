#ifndef BITTER_COW_H
#define BITTER_COW_H

#include "format/bitterfs_format.h"
#include "core/bitter_env.h"
#include "core/btree.h"

int btree_cow_block(struct bitter_env* env, struct bitter_root* root, struct bitter_path* path, 
    bt_u8 level, struct bitter_trans* trans);



#endif
