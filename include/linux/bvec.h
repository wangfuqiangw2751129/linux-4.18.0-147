/*
 * bvec iterator
 *
 * Copyright (C) 2001 Ming Lei <ming.lei@canonical.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 *
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public Licens
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-
 */
#ifndef __LINUX_BVEC_ITER_H
#define __LINUX_BVEC_ITER_H

#include <linux/kernel.h>
#include <linux/bug.h>
#include <linux/errno.h>

/*
 * was unsigned short, but we might as well be ready for > 64kB I/O pages
 */
struct bio_vec {                //这个数据结构主要是对一个page中关于此缓冲区的描述
	struct page	*bv_page;       //数据所在页面的首地址
	unsigned int	bv_len;     //数据长度
	unsigned int	bv_offset;  //页面偏移量
};

struct bvec_iter {
	sector_t		bi_sector;	/* device address in 512 byte       //要操作的扇区号
						   sectors */
	unsigned int		bi_size;	/* residual I/O count */        //剩余处理的大小

	unsigned int		bi_idx;		/* current index into bvl_vec */ //当前的bio_vec索引号

	unsigned int            bi_bvec_done;	/* number of bytes completed in //当前bio_vec中已经完成的字节数
						   current bvec */
	/*
	 * FOR RH USE ONLY.
	 *
	 * The reserved field may help us in future, and it won't introduce
	 * extra real space actually.
	 */
	unsigned int		rh_bi_reserved;
};

/*
 * various member access, note that bio_data should of course not be used
 * on highmem page vectors
 */
#define __bvec_iter_bvec(bvec, iter)	(&(bvec)[(iter).bi_idx])

#define bvec_iter_page(bvec, iter)				\
	(__bvec_iter_bvec((bvec), (iter))->bv_page)

#define bvec_iter_len(bvec, iter)				\
	min((iter).bi_size,					\
	    __bvec_iter_bvec((bvec), (iter))->bv_len - (iter).bi_bvec_done)

#define bvec_iter_offset(bvec, iter)				\
	(__bvec_iter_bvec((bvec), (iter))->bv_offset + (iter).bi_bvec_done)

#define bvec_iter_bvec(bvec, iter)				\
((struct bio_vec) {						\
	.bv_page	= bvec_iter_page((bvec), (iter)),	\
	.bv_len		= bvec_iter_len((bvec), (iter)),	\
	.bv_offset	= bvec_iter_offset((bvec), (iter)),	\
})
//这个相当于把iter迭代器前进bytes字节
static inline bool bvec_iter_advance(const struct bio_vec *bv,
		struct bvec_iter *iter, unsigned bytes)
{
	if (WARN_ONCE(bytes > iter->bi_size,        //bytes 不能 > iter->bi_size
		     "Attempted to advance past end of bvec iter\n")) {
		iter->bi_size = 0;
		return false;
	}

	while (bytes) {
		unsigned iter_len = bvec_iter_len(bv, *iter);       //这个是取当前bio_vec中剩余的字节数
		unsigned len = min(bytes, iter_len);

		bytes -= len;                                       //减去这个
		iter->bi_size -= len;
		iter->bi_bvec_done += len;                          //加上
        //如果==当前迭代器的->bv_len
		if (iter->bi_bvec_done == __bvec_iter_bvec(bv, *iter)->bv_len) {
			iter->bi_bvec_done = 0;
			iter->bi_idx++;                                 //index ++
		}
	}
	return true;
}

#define for_each_bvec(bvl, bio_vec, iter, start)			\
	for (iter = (start);						\
	     (iter).bi_size &&						\
		((bvl = bvec_iter_bvec((bio_vec), (iter))), 1);	\
	     bvec_iter_advance((bio_vec), &(iter), (bvl).bv_len))

/* for iterating one bio from start to end */
#define BVEC_ITER_ALL_INIT (struct bvec_iter)				\
{									\
	.bi_sector	= 0,						\
	.bi_size	= UINT_MAX,					\
	.bi_idx		= 0,						\
	.bi_bvec_done	= 0,						\
}

#endif /* __LINUX_BVEC_ITER_H */
