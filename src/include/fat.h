/**
 * @file fat.c
 * @brief fat file system implementation.
 * @author mopp
 * @version 0.1
 * @date 2014-10-25
 */


#ifndef _FAT_H_
#define _FAT_H_



#include <state_code.h>
#include <fs.h>


struct bios_param_block {
    uint8_t jmp_boot[3];        /* Jump instruction for boot. */
    uint8_t oem_name[8];        /* System that makes this partition. */
    uint16_t bytes_per_sec;     /* byte size of one sector. */
    uint8_t sec_per_clus;       /* The number of sector per cluster, this is power of 2. */
    uint16_t rsvd_area_sec_num; /* The number of sector of reserved area sector  */
    uint8_t num_fats;           /* The number of FAT. */
    uint16_t root_ent_cnt;      /* This is only used by FAT12 or FAT16. */
    uint16_t total_sec16;       /* This is only used by FAT12 or FAT16. */
    uint8_t media;              /* This is not used. */
    uint16_t fat_size16;        /* This is only used by FAT12 or FAT16. */
    uint16_t sec_per_trk;       /* The number of sector of a track. */
    uint16_t num_heads;         /* The number of head. */
    uint32_t hidd_sec;          /* The number of physical sector that is located on before this volume. */
    uint32_t total_sec32;       /* Total sector num of FAT32 */
    union {
        struct {
            uint8_t drive_num;
            uint8_t reserved_1;
            uint8_t boot_sig;
            uint32_t volume_id;
            uint8_t volume_label[11];
            uint8_t fs_type[8];
        } fat16;
        struct {
            uint16_t fat_size32; /* The number of sector of one FAT. */
            union {
                uint16_t ext_flags;
                struct {
                    uint8_t active_fat : 4; /* This is enable when mirror_all_fat is 1. */
                    uint8_t ext_reserved0 : 3;
                    uint8_t is_active : 1; /* if this is 0, all FAT is mirroring, otherwise one FAT is only enable. */
                    uint8_t ext_reserved1;
                };
            };
            uint16_t fat32_version;   /* FAT32 volume version. */
            uint32_t rde_clus_num;    /* First cluster number of root directory entry. */
            uint16_t fsinfo_sec_num;  /* Sector number of FSINFO struct location in reserved area(always 1). */
            uint16_t bk_boot_sec;     /* Backup of boot sector sector number. */
            uint8_t reserved[12];     /* Reserved. */
            uint8_t drive_num;        /* BIOS drive number. */
            uint8_t reserved1;        /* Reserved. */
            uint8_t boot_sig;         /* Extend boot signature. */
            uint32_t volume_id;       /* Volume serial number. */
            uint8_t volume_label[11]; /* Partition volume label */
            uint8_t fs_type[8];
        } fat32;
    };
} __attribute__((packed));
typedef struct bios_param_block Bios_param_block;


/*
 * NOTE: free_cnt and next_free is not necessary correct.
 */
struct fsinfo {
    uint32_t lead_signature;   /* This is used to validate that this is in fact an FSInfo sector. */
    uint8_t reserved0[480];    /* must be zeros. */
    uint32_t struct_signature; /* This is more localized in the sector to the location of the fields that are used. */
    uint32_t free_cnt;         /* The last known free cluster count */
    uint32_t next_free;        /* The cluster number at which the driver should start looking for free clusters. */
    uint8_t reserved1[12];     /* must be zeros. */
    uint32_t trail_signature;  /* This is used to validate that this is in fact an FSInfo sector. */
};
typedef struct fsinfo Fsinfo;
_Static_assert(sizeof(Fsinfo) == 512, "Fsinfo is NOT 512 byte.");


/* Short file name formant. */
struct dir_entry {
    uint8_t name[11]; /* short name. */
    uint8_t attr;     /* attribute. */
    uint8_t nt_rsvd;  /* reserved by Windows NT. */
    uint8_t create_time_tenth;
    uint16_t create_time;
    uint16_t create_date;
    uint16_t last_access_date;
    uint16_t first_clus_num_hi;
    uint16_t write_time;
    uint16_t write_date;
    uint16_t first_clus_num_lo;
    uint32_t file_size;
};
typedef struct dir_entry Dir_entry;
_Static_assert(sizeof(Dir_entry) == 32, "Dir_entry is NOT 32 byte.");


/* Long file name formant. */
struct long_dir_entry {
    uint8_t order; /* The order of this entry in the sequence of long dir entries. */
    uint8_t name0[10];
    uint8_t attr;
    uint8_t type;
    uint8_t checksum;
    uint8_t name1[12];
    uint16_t first_clus_num_lo;
    uint8_t name2[4];
};
typedef struct long_dir_entry Long_dir_entry;
_Static_assert(sizeof(Long_dir_entry) == 32, "Long_dir_entry is NOT 32 byte.");


#define FSINFO_TRAIL_SIG         0xAA550000
#define FSINFO_INVALID_FREE      0xFFFFFFFF
enum {
    FSINFO_LEAD_SIG         = 0x41615252,
    FSINFO_STRUCT_SIG       = 0x61417272,
    DIR_LAST_FREE           = 0x00,
    DIR_FREE                = 0xe5,
    DIR_ATTR_READ_ONLY      = 0x01,
    DIR_ATTR_HIDDEN         = 0x02,
    DIR_ATTR_SYSTEM         = 0x04,
    DIR_ATTR_VOLUME_ID      = 0x08,
    DIR_ATTR_DIRECTORY      = 0x10,
    DIR_ATTR_ARCHIVE        = 0x20,
    DIR_ATTR_LONG_NAME      = DIR_ATTR_READ_ONLY | DIR_ATTR_HIDDEN | DIR_ATTR_SYSTEM | DIR_ATTR_VOLUME_ID,
    DIR_ATTR_LONG_NAME_MASK = DIR_ATTR_READ_ONLY | DIR_ATTR_HIDDEN | DIR_ATTR_SYSTEM | DIR_ATTR_VOLUME_ID | DIR_ATTR_DIRECTORY | DIR_ATTR_ARCHIVE,
    LFN_LAST_ENTRY_FLAG     = 0x40,
    FAT_TYPE12              = 12,
    FAT_TYPE16              = 16,
    FAT_TYPE32              = 32,
};


struct fat_file {
    File super;
    uint32_t clus_num;
};
typedef struct fat_file Fat_file;


struct fat_area {
    uint32_t begin_sec;
    uint32_t sec_nr;
};
typedef struct fat_area Fat_area;


struct fat_manager;
typedef struct fat_manager Fat_manager;
typedef Axel_state_code (*Dev_access)(Fat_manager const*, uint8_t, uint32_t, uint8_t, uint8_t*);


struct fat_manager {
    File_system super;
    Fsinfo fsinfo;
    Bios_param_block* bpb;
    uint8_t* buffer; /* this buffer has area that satisfy one cluster size. */
    size_t buffer_size;
    Fat_area rsvd;
    Fat_area fat;
    Fat_area rdentry;
    Fat_area data;
    Dev_access access;
    uint32_t cluster_nr;
    uint8_t fat_type;
};
typedef struct fat_manager Fat_manager;


File_system* init_fat(Ata_dev* dev, Partition_entry* pe);



#endif
