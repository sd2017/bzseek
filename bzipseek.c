#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#include <unistd.h>

#include <assert.h>

#include "bzipseek.h"

typedef uint64_t filepos_t;


#define ROUND_UP_BYTES(nbits) (((nbits)+7)/8)
static bzseek_err load_block(bzseek_file* f, filepos_t start, filepos_t end){
  filepos_t startbyte = start / 8;
  int start_off = start % 8;
  filepos_t nbits = end - start;
  int nbytes = ROUND_UP_BYTES(nbits);
  int nread = ROUND_UP_BYTES(end - startbyte * 8);
  int i;
  
  int mallocsize = nbytes + 100;
  if (f->bufsize < mallocsize){
    free(f->buf);
    f->buf = malloc(mallocsize);
    f->bufsize = mallocsize;
    if (!f->buf) return BZSEEK_OUT_OF_MEM;
  }
  
  unsigned char* buf = (unsigned char*)f->buf;

  memcpy(buf, "BZh0", 4);
  buf[3] = '0' + f->blocksz;
  
  unsigned char* bit_data = buf+4;

  fseeko(f->f_data, startbyte, SEEK_SET);
  fread(bit_data, 1, nread, f->f_data);

  
  unsigned char blk_header[10];
  for (i=0;i<10;i++){
    blk_header[i] = 
      (bit_data[i] << start_off) | 
      (bit_data[i+1] >> (8-start_off));
  }


  assert(!memcmp(blk_header, "\x31\x41\x59\x26\x53\x59", 6));

  memcpy(bit_data + nread, "\x17\x72\x45\x38\x50\x90", 6); 
  memcpy(bit_data + nread + 6, blk_header + 6, 4);

  int trail_off = end % 8;
  bit_data[nread + 10] = 0;
  bit_data[nread-1] = 
    ((bit_data[nread-1] >> (8-trail_off)) << (8-trail_off)) | 
    (bit_data[nread] >> trail_off);
  unsigned char* trailer = bit_data + nread;
  for (i=0;i<10;i++){
    trailer[i] = 
      (trailer[i] << (8-trail_off)) | 
      (trailer[i+1] >> trail_off);
  }


  for (i=0;i<nread+10;i++){
    bit_data[i] = 
      (bit_data[i] << start_off) |
      (bit_data[i+1] >> (8-start_off));
  }


  f->buflen = 4 + nbytes + 10;
  return BZSEEK_OK;
}

static bzseek_err init_bz(bzseek_file* f){
  /* the first time this is called, bz may be null, but BZ2_bzDecompressEnd
     doesn't care. */
  BZ2_bzDecompressEnd(&f->bz);
  memset(&f->bz, 0, sizeof(bz_stream));
  int err = BZ2_bzDecompressInit(&f->bz, 1, 0);
  f->bz.next_in = f->buf;
  f->bz.avail_in = f->buflen;
  
  if (err == BZ_OK) return BZSEEK_OK;
  if (err == BZ_MEM_ERROR) return BZSEEK_OUT_OF_MEM;
  return BZSEEK_USAGE_ERR;
}

static uint64_t idx_uncomp_pos(bzseek_file* f, int block){
  assert(0 <= block && block < f->idx_nitems && f->idx_data);
  return f->idx_data[block * 2 + 1];
}
static uint64_t idx_comp_pos(bzseek_file* f, int block){
  assert(0 <= block && block < f->idx_nitems && f->idx_data);
  return f->idx_data[block * 2];
}


static uint64_t get_bz_uncomp_pos(bzseek_file* f){
  uint64_t bzproduced = 
    (uint64_t)f->bz.total_out_lo32 +
    (((uint64_t)f->bz.total_out_hi32) << 32);
  return bzproduced + idx_uncomp_pos(f, f->curr_block);
}

bzseek_err run_bz(bzseek_file* f, int* count, int* len, char** buf){
  f->bz.next_out = *buf;
  f->bz.avail_out = *len;
  int oldlen = *len;
  int err = BZ2_bzDecompress(&f->bz);
  *buf = f->bz.next_out;
  *len = f->bz.avail_out;
  *count = oldlen - *len;

  switch (err){
  case BZ_PARAM_ERROR: 
    return BZSEEK_USAGE_ERR;
  case BZ_DATA_ERROR:
  case BZ_DATA_ERROR_MAGIC:
    return BZSEEK_BAD_DATA;
  case BZ_MEM_ERROR:
    return BZSEEK_OUT_OF_MEM;
  case BZ_STREAM_END:
    return BZSEEK_EOF;
  case BZ_OK:
    return BZSEEK_OK;
  default:
    assert(0);
  }
}






bzseek_err seek_to_pos(bzseek_file* f, uint64_t pos){
  /* If we've already loaded the right block, do nothing */
  if (f->curr_block != -1 && 
      idx_uncomp_pos(f, f->curr_block) <= pos && 
      pos < idx_uncomp_pos(f, f->curr_block + 1))
    return BZSEEK_OK;
  if (pos >= bzseek_len(f))
    return BZSEEK_EOF;

  int i = 0, j = f->idx_nitems;
  while (j - i != 1){
    int x = (i + j) / 2;
    if (idx_uncomp_pos(f, x) <= pos)
      i = x;
    else
      j = x;
  }
  assert(i < f->idx_nitems - 1);
  f->curr_block = i;

  bzseek_err err = BZSEEK_OK;
  if (err=load_block(f, idx_comp_pos(f, i), idx_comp_pos(f, i+1))) return err;
  if (err=init_bz(f)) return err;
  return BZSEEK_OK;
}

unsigned int parse_int32(unsigned char* x){
  int i;
  unsigned int n = 0;
  for (i=0; i<4; i++){
    n = (n << 8) + x[i];
  }
  return n;
}

uint64_t parse_int64(unsigned char* x){
  int i;
  uint64_t n = 0;
  for (i=0; i<8; i++){
    n = (n << 8) + x[i];
  }
  return n;
}

bzseek_err load_index(bzseek_file* f){
  int i;
  FILE* ix = f->f_idx;
  fseeko(ix, 0, SEEK_SET);
  unsigned char header[8] = {0};
  fread(header, 8, 1, ix);
  int size;
  if (!memcmp(header, "BZIX", 4)){
    size = (int)parse_int32(header + 4);
  }else{
    /* search at end of file */
    fseeko(ix, -8, SEEK_SET);
    fread(header, 8, 1, ix);
    if (!memcmp(header, "BZIX", 4)){
      size = (int)parse_int32(header + 4);
      fseeko(ix, -size + 8, SEEK_CUR);
    }else{
      return BZSEEK_BAD_INDEX;
    }
  }

  if (size < 16 || size > 16*100000){
    return BZSEEK_BAD_INDEX;
  }

  f->idx_nitems = (size - 16) / 16;
  f->idx_data = malloc(size - 16);
  fread(f->idx_data, 8, f->idx_nitems * 2, ix);
  for (i=0; i<f->idx_nitems; i++){
    f->idx_data[i*2] = parse_int64((unsigned char*)&f->idx_data[i*2]);
    /* uncompressed positions are also stored in bits, convert to bytes */
    f->idx_data[i*2+1] = parse_int64((unsigned char*)&f->idx_data[i*2+1]) / 8;
  }

  /* minor error checking: are the offsets increasing? */
  for (i=0; i<f->idx_nitems-1; i++){
    if (idx_comp_pos(f, i) >= idx_comp_pos(f,i+1) ||
        idx_uncomp_pos(f, i) >= idx_uncomp_pos(f, i+1))
      return BZSEEK_BAD_INDEX;
  }

  return BZSEEK_OK;
}



bzseek_err bzseek_open(bzseek_file* file, FILE* data_file, FILE* idx_file){
  if (!idx_file) idx_file = data_file;

  memset(file, 0, sizeof(file));
  file->f_data = data_file;
  file->f_idx = idx_file;

  file->curr_block = -1;

  fseek(data_file, 0, SEEK_SET);
  char header[4] = {0};
  fread(header, 1, 4, data_file);
  if (memcmp(header, "BZh", 3))
    return BZSEEK_BAD_DATA;
  if (header[3] >= '0' && header[3] <= '9')
    file->blocksz = header[3] - '0';
  else
    return BZSEEK_BAD_DATA;
  
  bzseek_err err;
  if (err=load_index(file)) return err;
  
  return BZSEEK_OK;
}


uint64_t bzseek_len(bzseek_file* file){
  return idx_uncomp_pos(file, file->idx_nitems - 1);
}


#define NULL_BUF_SZ 1024
bzseek_err bzseek_read(bzseek_file* file, uint64_t start, int len, char* buf){
  uint64_t end = start + (uint64_t)len;
  bzseek_err err = BZSEEK_OK;

  /* loop in case the request spans multiple blocks */
  while (len > 0){
    /* load the correct block */
    if (err = seek_to_pos(file, start)) return err;
    assert(idx_uncomp_pos(file, file->curr_block) <= start && 
           start < idx_uncomp_pos(file, file->curr_block + 1));

    /* we may need to seek inside the block */
    uint64_t bzpos = get_bz_uncomp_pos(file);

    if (bzpos > start){
      /* we need to rewind to the start of the current block */
      if (err = init_bz(file)) return err;
      bzpos = get_bz_uncomp_pos(file);
      assert(bzpos == idx_uncomp_pos(file, file->curr_block));
    }

    assert(bzpos <= start);

    if (bzpos < start){
      /* we need to seek ahead. Do so by decompressing some data into 
         an unused buffer. */
      char devnull[NULL_BUF_SZ];
      /* we know this fits in an int since start and bzpos are in the same block */
      int seek_forward = (int)(start - bzpos);
      while (seek_forward > 0){
        char* null_buf = devnull;
        int null_len = seek_forward > NULL_BUF_SZ ? NULL_BUF_SZ : seek_forward;
        int cnt;
        if (err = run_bz(file, &cnt, &null_len, &null_buf)) return err;
        seek_forward -= cnt;
      }
      bzpos = get_bz_uncomp_pos(file);
    }
   
    assert(bzpos == start);
    /* we're at the right place, do some decompression */
    int cnt;
    if (err = run_bz(file, &cnt, &len, &buf)) return err;
    start += cnt;
  }
  return BZSEEK_OK;
}


void bzseek_close(bzseek_file* f){
  free(f->buf);
  f->buf = NULL;
  fclose(f->f_data);
  f->f_data = NULL;
  fclose(f->f_idx);
  f->f_idx = NULL;
  BZ2_bzDecompressEnd(&f->bz);
}

const char* bzseek_errmsg(bzseek_err err){
  switch (err){
  case BZSEEK_OK:         return "Success";
  case BZSEEK_OUT_OF_MEM: return "Out of memory";
  case BZSEEK_BAD_INDEX:  return "Error reading index";
  case BZSEEK_BAD_DATA:   return "Malformed bzip2 data";
  case BZSEEK_USAGE_ERR:  return "Error";
  case BZSEEK_IO_ERR:     return "I/O Error";
  case BZSEEK_EOF:        return "End of file";
  default:                return "Unknown error";
  }
}

int main(int argc, char* argv[]){
  //  bunzip_block(9, open("testfile.bz2", O_RDONLY), 32, 6458889 /*6458866*/);
  //  bunzip_block(9, open("test3.bz2", O_RDONLY), atoi(argv[1]), atoi(argv[2]));
  bzseek_file f;
  //  f.f_data = f.f_idx = fopen("index","r");
  //  load_index(&f);
  bzseek_open(&f, fopen("test3.bz2", "r"), fopen("index","r"));
  char x[100000];
  bzseek_err err;
  if (err = bzseek_read(&f, atoi(argv[1]), atoi(argv[2]), x)){
    printf("error: %s\n" , bzseek_errmsg(err));
  }
  fwrite(x, 1, atoi(argv[2]), stdout);
  bzseek_close(&f);
  return 0;
}
