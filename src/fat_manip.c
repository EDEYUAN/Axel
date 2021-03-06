#include "./include/mbr.h"
#include "./include/fat_manip.h"
#include "./include/macros.h"


#ifdef FOR_IMG_UTIL



/* This is used in img_util */
#include <string.h>
#include <time.h>
#include <stdio.h>
#include <ctype.h>



#else



/* This is used in Axel */
#include <utils.h>



#endif



static inline void* alloc_clus_buf(Fat_manips const* fm) {
    return fm->alloc(fm->byte_per_cluster);
}


static inline void* clear_clus_buf(Fat_manips const* fm, void* buffer) {
    return memset(buffer, 0, fm->byte_per_cluster);
}


/**
 * @brief Access FAT entry.
 * @param fm manipulator
 * @param direction read or write
 * @param n FAT Number to read/write
 * @param write_entry It is enable when writting.
 * @return Value of FAT entry.
 */
uint32_t fat_entry_access(Fat_manips const* fm, uint8_t direction, uint32_t n, uint32_t write_entry) {
    uint32_t fat_type     = fm->fat_type;
    uint32_t offset       = GET_VALUE_BY_FAT_TYPE(fm->fat_type, (n + n / 2), (n * 2), (n * 4));
    uint32_t bps          = fm->bpb->bytes_per_sec;
    uint32_t sec_num      = fm->bpb->rsvd_area_sec_num + (offset / bps);
    uint32_t entry_offset = offset % bps;
    void* buf = fm->alloc(bps * 2);
    if (buf == NULL) {
        return 0;
    }

    if (fm->b_access(fm->obj, FILE_READ, sec_num, 2, buf) != AXEL_SUCCESS) {
        fm->free(buf);
        return 0;
    }
    uint32_t* target_entry = (uint32_t*)((uintptr_t)buf + entry_offset);
    uint32_t read_entry = 0;

    /* Filter */
    switch (fat_type) {
        case FAT_TYPE12:
            if ((n & 1) == 1) {
                /* Odd entry number. */
                read_entry = (*target_entry >> 4 ) & 0x00000FFF;
            } else {
                /* Even entry number. */
                read_entry = *target_entry & 0x00000FFF;
            }
            break;
        case FAT_TYPE16:
            read_entry = *target_entry & 0x0000FFFF;
            break;
        case FAT_TYPE32:
            read_entry = *target_entry & 0x0FFFFFFF;
            break;
    }

    if (direction == FILE_READ) {
        fm->free(buf);
        return read_entry;
    }

    /* Change FAT entry. */
    switch (fat_type) {
        case FAT_TYPE12:
            if ((n & 1) == 1) {
                /* Odd entry number. */
                *target_entry = ((write_entry & 0xFFF) << 4) | (*target_entry & 0xFFFF000F);
            } else {
                /* Even entry number. */
                *target_entry = (write_entry & 0xFFF) | (*target_entry & 0xFFFFF000);
            }
            break;
        case FAT_TYPE16:
            *target_entry = (write_entry & 0xFFFF) | (*target_entry & 0xFFFF0000);
            break;
        case FAT_TYPE32:
            /* keep upper bits */
            *target_entry = (*target_entry & 0xF0000000) | (write_entry & 0x0FFFFFFF);
            break;
    }

    if (fm->b_access(fm->obj, FILE_WRITE, sec_num, 2, buf) != AXEL_SUCCESS) {
        fm->free(buf);
        return 0;
    }

    return *target_entry;
}


uint32_t fat_entry_read(Fat_manips const* fm, uint32_t n) {
    return fat_entry_access(fm, FILE_READ, n, 0);
}


uint32_t fat_entry_write(Fat_manips const* fm, uint32_t n, uint32_t write_entry) {
    return fat_entry_access(fm, FILE_WRITE, n, write_entry);
}


uint32_t set_last_fat_entry(Fat_manips const* fm, uint32_t n) {
    uint32_t last = 0;

    switch (fm->fat_type) {
        case FAT_TYPE12:
            last = 0xFFF;
        case FAT_TYPE16:
            last = 0xFFFF;
        case FAT_TYPE32:
            last = 0x0FFFFFFF;
    }

    return fat_entry_write(fm, n, last);
}


Fsinfo* fat_fsinfo_access(Fat_manips const* fm, uint8_t direction, Fsinfo* fsi) {
    void* buf = alloc_clus_buf(fm);
    if (buf == NULL) {
        return NULL;
    }

    if (direction == FILE_WRITE) {
        memcpy(buf, fsi, sizeof(Fsinfo));
    }

    /* Access FSINFO structure. */
    if (fm->b_access(fm->obj, direction, fm->bpb->fat32.fsinfo_sector, 1, buf) != AXEL_SUCCESS) {
        fm->free(buf);
        return NULL;
    }

    if (direction == FILE_READ) {
        memcpy(fsi, buf, sizeof(Fsinfo));
    }

    fm->free(buf);

    return fsi;
}


uint8_t* fat_data_cluster_access(Fat_manips const* fm, uint8_t direction, uint32_t cluster, uint8_t* buffer) {
    uint8_t spc = fm->bpb->sec_per_clus;
    uint32_t sec = fm->area.data.begin_sec + ((cluster - 2) * spc);

    if (fm->b_access(fm->obj, direction, sec, spc, buffer) != AXEL_SUCCESS) {
        return NULL;
    }

    return buffer;
}


uint8_t* fat_root_dir_area_access(Fat_manips const* fm, uint8_t direction, uint32_t sector, uint8_t* buffer) {
    if (fm->b_access(fm->obj, direction, fm->area.rdentry.begin_sec + sector, fm->bpb->sec_per_clus, buffer) != AXEL_SUCCESS) {
        return NULL;
    }

    return buffer;
}


uint32_t alloc_cluster(Fat_manips* fm) {
    if (fm->fat_type != FAT_TYPE32) {
        /*
         * FAT12/16
         * FAT entry begin at index 2 (0 and 1 is reserved).
         */
        for (uint32_t i = 2; i < fm->max_fat_entry_number; i++) {
            if (is_unused_fat_entry(fat_entry_read(fm, i)) == true) {
                return i;
            }
        }
        return AXEL_FAILED;
    }

    /* FAT32 */
    if (fm->fsinfo->free_cnt == 0) {
        /* No free cluster. */
        return 0;
    }

    uint32_t begin = fm->fsinfo->next_free + 1;
    uint32_t end = fm->area.data.sec_nr / fm->bpb->sec_per_clus;
    uint32_t i, select_clus = 0;

    for (i = begin; i < end; i++) {
        uint32_t fe = fat_entry_read(fm, i);
        if (is_unused_fat_entry(fe) == true) {
            /* Found unused cluster. */
            select_clus = i;
            break;
        }
    }

    if (select_clus == 0) {
        /* Unused cluster is not found. */
        return 0;
    }

    /* Update FSINFO. */
    fm->fsinfo->next_free = select_clus;
    fm->fsinfo->free_cnt -= 1;
    fat_fsinfo_access(fm, FILE_WRITE, fm->fsinfo);

    return select_clus;
}


void free_cluster(Fat_manips* fm, uint32_t clus) {
    uint32_t fe;
    do {
        fe = fat_entry_read(fm, clus);
        bool f = is_unused_fat_entry(fe);
        if (f == false || is_last_fat_entry(fm, fe) == true) {
            fat_entry_write(fm, clus, 0);
        }

        if (f == true) {
            return;
        }
        clus = fe;
    } while (1);
}


static inline void set_long_file_name(uint8_t* dst_p, char const** src_p, char const* src_last, uint8_t dst_max_len_16, bool* padding_flag) {
    uint16_t* dst = (void*)dst_p;
    char const* src = *src_p;

    for (uint8_t j = 0; j < dst_max_len_16; j++) {
        if (src == src_last) {
            if (*padding_flag == true) {
                dst[j] = 0xFFFF;
            } else {
                /* Set terminal as null character. */
                dst[j] = 0;
                *padding_flag = true;
            }
        } else {
            /* Supporting only ascii character. */
            dst[j] = ((*src++) & 0x00FF);
        }
    }
    *src_p = src;
}


uint8_t fat_calc_checksum(Dir_entry const* entry) {
    uint8_t sum = 0;

    for (uint8_t i = 0; i < 11; i++) {
        sum = (sum >> 1u) + (uint8_t)(sum << 7u) + entry->name[i];
    }

    return sum;
}


static inline size_t ucs2_to_ascii(uint8_t* ucs2, char* ascii, size_t ucs2_len) {
    if ((ucs2_len & 0x1) == 1) {
        /* invalid. */
        return 0;
    }

    size_t cnt = 0;
    for (size_t i = 0; i < ucs2_len; i++) {
        uint8_t c = ucs2[i];
        if ((0 < c) && (c < 127)) {
            ascii[cnt++] = (char)ucs2[i];
        }
    }
    ascii[cnt] = '\0';

    return cnt;
}


char* fat_get_long_dir_name(Long_dir_entry* long_entry, char* buffer, size_t* index) {
    *index += ucs2_to_ascii(long_entry->name0, buffer + *index, 10);
    *index += ucs2_to_ascii(long_entry->name1, buffer + *index, 12);
    *index += ucs2_to_ascii(long_entry->name2, buffer + *index, 4);
    return buffer;
}


static inline bool is_83_format(char const* name) {
    if (12 <= strlen(name)) {
        return false;
    }
    return true;
}


static inline char* create_short_file_name(char const* name, char* sfn_buffer) {
    bool irreversible_conversion_flag = false;
    size_t len = strlen(name);
    char work_buffer[FAT_FILE_MAX_NAME_LEN];
    strcpy(work_buffer, name);

    /* Remove head period. */
    for (size_t i = 0; i < len; ++i) {
        if (work_buffer[i] != '.') {
            break;
        }
        work_buffer[i] = ' ';
        irreversible_conversion_flag = true;
    }

    /* Change period to space without last period. */
    bool is_last_period = false;
    for (size_t i = len; 0 < i; --i) {
        size_t idx = i - 1;
        if (work_buffer[idx] == '.') {
            if (is_last_period == false) {
                is_last_period = true;
            } else {
                work_buffer[idx] = ' ';
                irreversible_conversion_flag = true;
            }
        } else if (islower(work_buffer[idx]) != 0) {
            work_buffer[idx] = (char)toupper(work_buffer[idx]);
            irreversible_conversion_flag = true;
        }
    }

    /* Prepare SFN buffer. */
    memset(sfn_buffer, ' ', 11);

    /* Copy SFN body. */
    size_t sfn_copy_cnt = 0;
    size_t i;
    for (i = 0; i < len; i++) {
        char c = work_buffer[i];
        if (c == ' ') {
            continue;
        }

        if (c == '.' || sfn_copy_cnt == 8) {
            if (i < (len - 1)) {
                /* Remaining body part. */
                irreversible_conversion_flag = true;
            }
            break;
        }

        sfn_buffer[sfn_copy_cnt++] = c;
    }

    /* Search extension part. */
    if (work_buffer[i] != '.') {
        for (; i < len; i++) {
            if (work_buffer[i] == '.') {
                break;
            }
        }
    }

    /* Copy SFN extension. */
    for (; i < len; i++) {
        if (work_buffer[i] != ' ') {
            sfn_buffer[sfn_copy_cnt++] = work_buffer[i];
        }
    }

    if (irreversible_conversion_flag == true) {
        // /* FIXME: check filename collision. */
        // char number = '1';
        // /* Search space */
        // if (8 <= sfn_copy_cnt) {
        //     sfn_buffer[6] = '~';
        //     sfn_buffer[7] = number;
        // } else {
        //     sfn_buffer[sfn_copy_cnt] = '~';
        //     sfn_buffer[sfn_copy_cnt + 1] = number;
        //     sfn_copy_cnt += 2;
        // }
    }

    return sfn_buffer;
}


#ifdef FOR_IMG_UTIL
static inline void set_fat_time(uint16_t* fat_time) {
    time_t current_time = time(NULL);
    struct tm const *t_st = localtime(&current_time);

    /* Set time */
    uint16_t tmp = 0;
    tmp |= (0x001F & (t_st->tm_sec >> 1));  /* sec mod by 2 */
    tmp |= (0x07E0 & (t_st->tm_min << 5));
    tmp |= (0xF800 & (t_st->tm_hour << 11));
    *fat_time = tmp;
}


static inline void set_fat_date(uint16_t* date) {
    time_t current_time = time(NULL);
    struct tm const *t_st = localtime(&current_time);

    /* Set date */
    uint16_t tmp = 0;
    tmp |= (0x001F & (t_st->tm_mday));
    tmp |= (0x01E0 & ((t_st->tm_mon + 1) << 5));
    tmp |= (0xFE00 & ((t_st->tm_year + 1900 - 1980) << 9)); /* Base of tm_year is 1900. However, Base of FAT date is 1980. */
    *date = tmp;
}
#endif


static inline void init_short_dir_entry(Dir_entry* short_dir, uint32_t first_cluster, uint8_t attr, uint32_t size) {
    memset(short_dir, 0, sizeof(Dir_entry));
    short_dir->attr = attr;
    short_dir->first_clus_num_hi = (first_cluster >> 16) & 0xFFFF;
    short_dir->first_clus_num_lo = first_cluster & 0xFFFF;
    short_dir->file_size = size;
#ifdef FOR_IMG_UTIL
    set_fat_date(&short_dir->last_access_date);
    set_fat_date(&short_dir->create_date);
    set_fat_time(&short_dir->create_time);
    set_fat_date(&short_dir->write_date);
    set_fat_time(&short_dir->write_time);
#endif
}


/**
 * @brief
 * @param  fm
 * @param  parent_dir_cluster If parend directory is root directory in FAT16/12, It should be 0.
 * @param  name File name
 * @param  attr File attribute
 * @param  file_content
 * @param  file_size
 * @return
 */
Axel_state_code fat_create_file(Fat_manips* fm, uint32_t const parent_dir_cluster, char const* name, uint8_t attr, void* file_content, size_t const file_size) {
    uint32_t const fat_type         = fm->fat_type;
    size_t const name_len           = strlen(name);
    uint32_t const root_dir_cluster = fat_get_root_dir_cluster(fm);
    bool const is_parent_root = (root_dir_cluster == parent_dir_cluster) ? (true) : (false);

    if (fat_type == FAT_TYPE32) {
        if (FAT_FILE_MAX_NAME_LEN < name_len) {
            return AXEL_FAILED;
        }
    } else {
        /* Check 8.3 format */
        if (is_83_format(name) == false) {
            return AXEL_FAILED;
        }
    }

    uint32_t new_short_entry_cluster = alloc_cluster(fm);
    if (new_short_entry_cluster == 0) {
        return AXEL_FAILED;
    }

    set_last_fat_entry(fm, new_short_entry_cluster);

    /* Alloc new directory entry. */
    Dir_entry new_short_entry;
    init_short_dir_entry(&new_short_entry, new_short_entry_cluster, attr, (uint32_t)file_size);
    create_short_file_name(name, (char*)new_short_entry.name);

    /* Lond directory entry exists in only FAT32. */
    uint8_t long_dir_entry_nr = 0;
    if (fat_type == FAT_TYPE32) {
        /* Calculate the number of long dir entry to used */
        long_dir_entry_nr = (uint8_t)(name_len / FAT_LONG_DIR_MAX_NAME_LEN);
        if (name_len % FAT_LONG_DIR_MAX_NAME_LEN != 0) {
            ++long_dir_entry_nr;
        }
    }

    Long_dir_entry new_long_entries[long_dir_entry_nr];
    memset(new_long_entries, 0, sizeof(Long_dir_entry) * long_dir_entry_nr);
    if (fat_type == FAT_TYPE32) {
        /* Create new long directory entries. */
        char const* const name_last = name + name_len;
        bool padding_flag = false;
        uint8_t const checksum = fat_calc_checksum(&new_short_entry);
        for (uint8_t i = 0; i < long_dir_entry_nr; i++) {
            new_long_entries[i].order = i + 1;
            new_long_entries[i].attr = DIR_ATTR_LONG_NAME;
            new_long_entries[i].type = 0;
            new_long_entries[i].first_clus_num_lo = 0;
            new_long_entries[i].checksum = checksum;
            set_long_file_name(new_long_entries[i].name0, &name, name_last, 5, &padding_flag);
            set_long_file_name(new_long_entries[i].name1, &name, name_last, 6, &padding_flag);
            set_long_file_name(new_long_entries[i].name2, &name, name_last, 2, &padding_flag);
        }
        new_long_entries[long_dir_entry_nr - 1].order |= LFN_LAST_ENTRY_FLAG;
    }

    void* buffer = alloc_clus_buf(fm);
    if (buffer == NULL) {
        return AXEL_FAILED;
    }

    size_t const byte_per_sector = fm->bpb->bytes_per_sec;
    size_t const dentry_per_sector = byte_per_sector / sizeof(Dir_entry);

    /* Search location to store new entries. */
    /* FIXME: Used two clusters case */
    if (fat_type != FAT_TYPE32 && is_parent_root == true) {
        /* Access root directory entry. */

        /* Search free entry. */
        uint32_t const max_root_dir_sector_size = fm->area.rdentry.sec_nr;
        bool is_found_free_entry = false;
        uint32_t free_entry_contain_sector = 0;
        for (uint32_t sector = 0; (sector < max_root_dir_sector_size) && (is_found_free_entry == false); sector++) {
            if (fat_root_dir_area_access(fm, FILE_READ, sector, buffer) == NULL) {
                fm->free(buffer);
                return AXEL_FAILED;
            }

            Dir_entry* short_dentry_table = buffer;
            for (size_t i = 0; i < dentry_per_sector; i++) {
                uint8_t signature = short_dentry_table[i].name[0];
                if ((signature == DIR_FREE) || (signature == DIR_LAST_FREE)) {
                    short_dentry_table[i] = new_short_entry;
                    is_found_free_entry = true;
                    free_entry_contain_sector = sector;
                    break;
                }
            }
        }

        if (is_found_free_entry == false) {
            fm->free(buffer);
            return AXEL_FAILED;
        }

        if (fat_root_dir_area_access(fm, FILE_WRITE, free_entry_contain_sector, buffer) == NULL) {
            fm->free(buffer);
            return AXEL_FAILED;
        }
    } else {
        /* Access normal directory entry. */
        bool is_found_enough_entries = false;
        Dir_entry* short_dentry_table;
        size_t const request_entry_num = long_dir_entry_nr + 1; /* " + 1" means that adding short directory entry. */
        size_t free_entry_start_index = 0;
        uint32_t cluster_number;
        uint32_t fat_entry = parent_dir_cluster;
        while (is_found_enough_entries == false) {
            /* Check FAT entry. */
            cluster_number = fat_entry;
            fat_entry = fat_entry_read(fm, cluster_number);
            if (is_valid_data_exist_fat_entry(fm, fat_entry) == false) {
                fm->free(buffer);
                return AXEL_FAILED;
            }

            bool is_last_entry = false;
            if (is_last_fat_entry(fm, fat_entry) == true) {
                is_last_entry = true;
            }

            /* Load directory entry table. */
            if (fat_data_cluster_access(fm, FILE_READ, cluster_number, buffer) == NULL) {
                fm->free(buffer);
                return AXEL_FAILED;
            }

            /* Search enough free space. */
            short_dentry_table = buffer;
            size_t unused_entry_count = 0;
            for (size_t i = 0; i < dentry_per_sector; i++) {
                uint8_t signature = short_dentry_table[i].name[0];
                if (signature == DIR_FREE) {
                    ++unused_entry_count;
                    if (unused_entry_count == request_entry_num) {
                        free_entry_start_index = i;
                        is_found_enough_entries = true;
                        break;
                    }
                } else if (signature == DIR_LAST_FREE) {
                    /* This signature indicates that all remain entry is free. */
                    size_t remain_free_entry = dentry_per_sector - i;
                    if (request_entry_num <= (unused_entry_count + remain_free_entry)) {
                        free_entry_start_index = i + (request_entry_num - unused_entry_count - 1);
                        is_found_enough_entries = true;
                        break;
                    }
                }
            }

            if (is_last_entry == true && is_found_enough_entries == false) {
                /* Alloc new cluster to expand. */
                uint32_t prev_cluster = cluster_number;
                cluster_number = alloc_cluster(fm);
                fat_entry_write(fm, cluster_number, prev_cluster);
                set_last_fat_entry(fm, cluster_number);
            }
        }

        /*
         * Write short and long entries.
         * First, long entries are written by descending order (only FAT32).
         * Then, short entry are written.
         */
        if (fat_type == FAT_TYPE32) {
            Long_dir_entry* long_dentry_table = buffer;
            for (size_t i = long_dir_entry_nr; 0 < i; i--) {
                long_dentry_table[free_entry_start_index - i] = new_long_entries[i - 1];
            }
        }
        short_dentry_table[free_entry_start_index] = new_short_entry;
        fat_data_cluster_access(fm, FILE_WRITE, cluster_number, buffer);
    }


    if ((attr & DIR_ATTR_DIRECTORY) != 0) {
        /*
         * New entry is directory.
         * Create dot entries (".",  ".." and terminal entry) in new directory.
         * New directory should have these entry at head of its directory entry.
         */
        Dir_entry dot_entry[3];
        memset(dot_entry, 0, sizeof(Dir_entry) * 3);
        init_short_dir_entry(&dot_entry[0], new_short_entry_cluster, DIR_ATTR_DIRECTORY | DIR_ATTR_ARCHIVE, 0);
        init_short_dir_entry(&dot_entry[1], ((is_parent_root == true) ? (0) : (parent_dir_cluster)), DIR_ATTR_DIRECTORY | DIR_ATTR_ARCHIVE, 0);
        memcpy(dot_entry[0].name, ".          ", 11);
        memcpy(dot_entry[1].name, "..         ", 11);
        dot_entry[2].name[0] = DIR_LAST_FREE;

        /* Write two entries into directory entry of new directory. */
        clear_clus_buf(fm, buffer);
        memcpy(buffer, dot_entry, sizeof(Dir_entry) * 3);
        void* result = fat_data_cluster_access(fm, FILE_WRITE, new_short_entry_cluster, buffer);
        if (result == NULL) {
            fm->free(buffer);
            return AXEL_FAILED;
        }
    } else {
        /*
         * New entry is file.
         * Writing contents.
         */
        size_t enough_cluster_size = file_size / fm->byte_per_cluster;
        if ((file_size % fm->byte_per_cluster) != 0) {
            ++enough_cluster_size;
        }
        size_t const byte_per_cluster = fm->byte_per_cluster;

        /* Write first cluster. */
        char* fc = (char*)file_content;
        clear_clus_buf(fm, buffer);
        memcpy(buffer, &fc[0], MIN(byte_per_cluster, file_size));
        void* r = fat_data_cluster_access(fm, FILE_WRITE, new_short_entry_cluster, buffer);
        if (r == NULL) {
            fm->free(buffer);
            return AXEL_FAILED;
        }

        /* Write remain clusters. */
        uint32_t prev_cluster = new_short_entry_cluster;
        for (size_t written_size = byte_per_cluster; written_size < file_size; written_size += byte_per_sector) {
            uint32_t new_cluster = alloc_cluster(fm);
            if (new_cluster == 0) {
                /* Allocation error. */
                return AXEL_FAILED;
            }

            /* Concatenate FAT chain. */
            fat_entry_write(fm, prev_cluster, new_cluster);
            set_last_fat_entry(fm, new_cluster);
            prev_cluster = new_cluster;

            /* Write content. */
            size_t remain_size = file_size - written_size;
            memcpy(buffer, &fc[written_size], MIN(byte_per_cluster, remain_size));
            void* r = fat_data_cluster_access(fm, FILE_WRITE, new_cluster, buffer);
            if (r == NULL) {
                fm->free(buffer);
                return AXEL_FAILED;
            }
        }
        set_last_fat_entry(fm, prev_cluster);
    }

    fm->free(buffer);

    return AXEL_SUCCESS;
}


Axel_state_code fat_make_directory(Fat_manips* fm, uint32_t parent_dir_cluster, char const* name, uint8_t attr) {
    return fat_create_file(fm, parent_dir_cluster, name, attr | DIR_ATTR_DIRECTORY | DIR_ATTR_ARCHIVE, NULL, 0);
}


Axel_state_code fat_find_file_short_entry(Fat_manips* fm, uint32_t dir_cluster, char const* name, Dir_entry* short_entry_dst) {
    short_entry_dst->name[0] = 0;
    if ((fm->fat_type != FAT_TYPE32) && (dir_cluster == fat_get_root_dir_cluster(fm))) {
        /* Case of FAT12/16 root directory. */
        char short_file_name[11];
        create_short_file_name(name, short_file_name);

        uint32_t const max_root_dir_sector_size = fm->area.rdentry.sec_nr;
        size_t const dentry_per_sector = fm->bpb->bytes_per_sec / sizeof(Dir_entry);
        void* buffer = alloc_clus_buf(fm);
        if (buffer == NULL) {
            return AXEL_FAILED;
        }

        bool is_found = false;
        for (uint32_t sector = 0; (sector < max_root_dir_sector_size) && (is_found == false); sector++) {
            if (fat_root_dir_area_access(fm, FILE_READ, sector, buffer) == NULL) {
                fm->free(buffer);
                return AXEL_FAILED;
            }

            Dir_entry* short_dentry_table = buffer;
            for (size_t i = 0; i < dentry_per_sector; i++) {
                uint8_t signature = short_dentry_table[i].name[0];
                if (signature == DIR_FREE) {
                    continue;
                } else if (signature == DIR_LAST_FREE) {
                    /* Not found. */
                    is_found = true;
                    break;
                } else if (memcmp(short_file_name, short_dentry_table[i].name, 11) == 0) {
                    /* Found. */
                    is_found = true;
                    *short_entry_dst = short_dentry_table[i];
                    break;
                }
            }
        }

        fm->free(buffer);
        return AXEL_SUCCESS;
    }

    if (is_valid_data_exist_fat_entry(fm, dir_cluster) == false) {
        return 0;
    }

    size_t const max_dentry_num = fm->byte_per_cluster / sizeof(Dir_entry);
    uint32_t entry = dir_cluster;
    void* buffer = alloc_clus_buf(fm);
    bool is_finish = false;
    while (is_finish == false) {
        if (NULL == fat_data_cluster_access(fm, FILE_READ, entry, buffer)) {
            fm->free(buffer);
            return AXEL_FAILED;
        }
        Dir_entry* short_dentry_table = buffer;
        Long_dir_entry* long_dentry_table = buffer;

        for (size_t i = 0; i < max_dentry_num; i++) {
            Dir_entry* short_entry = &short_dentry_table[i];
            uint8_t signature = short_entry->name[0];
            if (signature == DIR_FREE) {
                break;
            } else if (signature == DIR_LAST_FREE) {
                /* All remaining entry are free.  */
                is_finish = true;
                break;
            }

            if (is_lfn(short_entry) == false) {
                /* Short entry. */
                uint8_t checksum = fat_calc_checksum(short_entry);

                /* Check long entries. */
                char file_name[FAT_FILE_MAX_NAME_LEN];
                size_t index = 0;
                for (size_t j = i; 0 < j; --j) {
                    Long_dir_entry* long_entry = &long_dentry_table[j - 1];
                    if (long_entry->checksum != checksum) {
                        /* Checksum error. */
                        return AXEL_FAILED;
                    }
                    fat_get_long_dir_name(long_entry, file_name, &index);
                }

                if (strcmp(file_name, name) == 0) {
                    /* Found file. */
                    is_finish = true;
                    *short_entry_dst = *short_entry;
                    break;
                }
            }
        }

        uint32_t new_entry = fat_entry_read(fm, entry);
        if (is_last_fat_entry(fm, new_entry)) {
            is_finish = true;
        }
    }

    return AXEL_SUCCESS;
}


uint32_t fat_get_root_dir_cluster(Fat_manips* fm) {
    return GET_VALUE_BY_FAT_TYPE(fm->fat_type, FAT12_ROOT_DIR_CLUSTER_SIGNATURE, FAT16_ROOT_DIR_CLUSTER_SIGNATURE, fm->bpb->fat32.root_dentry_cluster);
}


bool is_valid_fsinfo(Fsinfo* fsi) {
    return ((fsi->lead_signature != FSINFO_LEAD_SIG) || (fsi->struct_signature != FSINFO_STRUCT_SIG) || (fsi->trail_signature != FSINFO_TRAIL_SIG)) ? false : true;
}


bool is_unused_fat_entry(uint32_t fe) {
    return (fe == 0) ? true : false;
}


bool is_rsvd_fat_entry(uint32_t fe) {
    return (fe == 1) ? true : false;
}


bool is_bad_clus_fat_entry(Fat_manips const* fm, uint32_t fe) {
    switch (fm->fat_type) {
        case FAT_TYPE12:
            return (fe == 0xFF7) ? true : false;
        case FAT_TYPE16:
            return (fe == 0xFFF7) ? true : false;
        case FAT_TYPE32:
            return (fe == 0x0FFFFFF7) ? true : false;
    }

    return false;
}


bool is_valid_data_fat_entry(Fat_manips const* fm, uint32_t fe) {
    /* NOT bad cluster and reserved. */
    return (is_bad_clus_fat_entry(fm, fe) == false && is_rsvd_fat_entry(fe) == false) ? true : false;
}


bool is_valid_data_exist_fat_entry(Fat_manips const* fm, uint32_t fe) {
    return (is_unused_fat_entry(fe) == false && is_valid_data_fat_entry(fm, fe) == true) ? true : false;
}


bool is_last_fat_entry(Fat_manips const* fm, uint32_t fe) {
    switch (fm->fat_type) {
        case FAT_TYPE12:
            return (0xFF8 <= fe && fe <= 0xFFF) ? true : false;
        case FAT_TYPE16:
            return (0xFFF8 <= fe && fe <= 0xFFFF) ? true : false;
        case FAT_TYPE32:
            return (0x0FFFFFF8 <= fe && fe <= 0x0FFFFFFF) ? true : false;
    }

    /* ERROR. */
    return false;
}


Fat_area* fat_calc_sectors(Bios_param_block const* bpb, Fat_area* fa) {
    /* These are logical sector number. */
    uint32_t fat_sector_size = (bpb->fat_sector_size16 != 0) ? (bpb->fat_sector_size16) : (bpb->fat32.fat_sector_size32);
    uint32_t total_sec = (bpb->total_sec16 != 0) ? (bpb->total_sec16) : (bpb->total_sec32);

    fa->rsvd.begin_sec = 0;
    fa->rsvd.sec_nr = bpb->rsvd_area_sec_num;

    fa->fat.begin_sec = fa->rsvd.begin_sec + fa->rsvd.sec_nr;
    fa->fat.sec_nr = fat_sector_size * bpb->fat_area_num;

    fa->rdentry.begin_sec = fa->fat.begin_sec + fa->fat.sec_nr;
    fa->rdentry.sec_nr = (32 * bpb->root_ent_cnt + bpb->bytes_per_sec - 1) / bpb->bytes_per_sec;

    fa->data.begin_sec = fa->rdentry.begin_sec + fa->rdentry.sec_nr;
    fa->data.sec_nr = total_sec - fa->data.begin_sec;

    return fa;
}


bool is_lfn(Dir_entry const* de) {
    return ((de->attr & DIR_ATTR_LONG_NAME_MASK) == DIR_ATTR_LONG_NAME) ? true : false;
}
