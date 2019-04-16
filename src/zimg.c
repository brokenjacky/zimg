/*
 *   zimg - high performance image storage and processing system.
 *       http://zimg.buaa.us
 *
 *   Copyright (c) 2013-2014, Peter Zhao <zp@buaa.us>.
 *   All rights reserved.
 *
 *   Use and distribution licensed under the BSD license.
 *   See the LICENSE file for full text.
 *
 */

/**
 * @file zimg.c
 * @brief Convert, get and save image functions.
 * @author 招牌疯子 zp@buaa.us
 * @version 3.0.0
 * @date 2014-08-14
 */

#include <stdio.h>
#include <sys/file.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>
#include <wand/magick_wand.h>
#include "zimg.h"
#include "zmd5.h"
#include "zlog.h"
#include "zcache.h"
#include "zutil.h"
#include "zdb.h"
#include "zscale.h"
#include "zhttpd.h"
#include "zlscale.h"
#include "cjson/cJSON.h"

typedef struct {
	evhtp_request_t *req;
	thr_arg_t *thr_arg;
	char address[16];
	int partno;
	int succno;
	int check_name;
	int flag;		// 1 filepath is ready
	char * filename;
	char * filepath;
	int width;

} mp_arg_t;

int save_img(mp_arg_t *p, const char *buff, const int len, char *md5);
int new_img(const char *buff, const size_t len, const char *save_name);
int get_img(zimg_req_t *req, evhtp_request_t *request);
int admin_img(evhtp_request_t *req, thr_arg_t *thr_arg, char *md5, int t);
int info_img(evhtp_request_t *request, thr_arg_t *thr_arg, char *md5);


int is_timg(const char *filename) {

	char * ext = strstr(filename, ".");
	if (ext)
	{
		ext = ext + 1;
		if (strcmp(ext,"jpeg")==0 || strcmp(ext,"jpg")==0 || strcmp(ext,"png")==0 || strcmp(ext,"webp")==0)
		{
			return 1;
		}	
	}
	return -1;
}

/**
 * @brief save_img Save buffer from POST requests
 *
 * @param thr_arg Thread arg struct.
 * @param buff The char * from POST request
 * @param len The length of buff
 * @param md5 Parsed md5 from url
 *
 * @return 1 for success and -1 for fail
 */
int save_img(mp_arg_t *p, const char *buff, const int len, char *md5) {
    int result = -1;

    LOG_PRINT(LOG_DEBUG, "Begin to Caculate MD5...%d",p->width);
   // md5_state_t mdctx;
    //md5_byte_t md_value[16];
    char md5sum[33];

    char save_path[512];
    char save_name[512];

    if (settings.mode != 1) {
        if (exist_db(p->thr_arg, md5sum) == 1) {
            LOG_PRINT(LOG_DEBUG, "File Exist, Needn't Save.");
            result = 1;
            goto done;
        }
        LOG_PRINT(LOG_DEBUG, "exist_db not found. Begin to Save File.");


        if (save_img_db(p->thr_arg, md5sum, buff, len) == -1) {
            LOG_PRINT(LOG_DEBUG, "save_img_db failed.");
            goto done;
        } else {
            LOG_PRINT(LOG_DEBUG, "save_img_db succ.");
            result = 1;
            goto done;
        }
    }

    //caculate 2-level path
    time_t t = time(NULL);
    struct tm *tt = localtime(&t);
    int lvl1 = (tt->tm_year+1900)*10000 + (tt->tm_mon+1)*100 + tt->tm_mday;

	int isTimg = is_timg(p->filename);
	if (isTimg<0)	// 非图片
	{
		if (p->filepath)
		{
			snprintf(save_path, 512, "%s/%s", settings.img_path, p->filepath);
		}
		else {
			snprintf(save_path, 512, "%s/%d", settings.img_path, lvl1);
		}
	} 
	else {
		if (p->filepath)
		{
			snprintf(save_path, 512, "%s/%s/src", settings.img_path, p->filepath);
		}
		else {
			snprintf(save_path, 512, "%s/%d/src", settings.img_path, lvl1);
		}
	}
	
    
    LOG_PRINT(LOG_DEBUG, "save_path: %s", save_path);

    if (is_dir(save_path) != 1) {
        if (mk_dirs(save_path) == -1) {
            LOG_PRINT(LOG_DEBUG, "save_path[%s] Create Failed!", save_path);
            goto done;
        }
        LOG_PRINT(LOG_DEBUG, "save_path[%s] Create Finish.", save_path);
    }

    snprintf(save_name, 512, "%s/%s",save_path, md5);
    LOG_PRINT(LOG_DEBUG, "save_name-->: %s", save_name);

	/*
    if (is_file(save_name) == 1) {
        LOG_PRINT(LOG_DEBUG, "Check File Exist. Needn't Save.");
        goto cache;
    }
	*/

    if (new_img(buff, len, save_name) == -1) {
        LOG_PRINT(LOG_DEBUG, "Save Image[%s] Failed!", save_name);
        goto done;
    }

done:
	if (isTimg>0)
	{
		snprintf(save_path, 512, "%s/%s", settings.img_path, p->filepath);
		save_timg(save_path, p);
	}
	result = 1;
    return result;
}

/**
 * @brief new_img The real function to save a image to disk.
 *
 * @param buff Const buff to write to disk.
 * @param len The length of buff.
 * @param save_name The name you want to save.
 *
 * @return 1 for success and -1 for fail.
 */
int new_img(const char *buff, const size_t len, const char *save_name) {
    int result = -1;
    LOG_PRINT(LOG_DEBUG, "Start to Storage the New Image...");
    int fd = -1;
    int wlen = 0;

    if ((fd = open(save_name, O_WRONLY | O_TRUNC | O_CREAT, 00644)) < 0) {
        LOG_PRINT(LOG_DEBUG, "fd(%s) open failed!", save_name);
        goto done;
    }

    if (flock(fd, LOCK_EX | LOCK_NB) == -1) {
        LOG_PRINT(LOG_DEBUG, "This fd is Locked by Other thread.");
        goto done;
    }

    wlen = write(fd, buff, len);
    if (wlen == -1) {
        LOG_PRINT(LOG_DEBUG, "write(%s) failed!", save_name);
        goto done;
    } else if (wlen < len) {
        LOG_PRINT(LOG_DEBUG, "Only part of [%s] is been writed.", save_name);
        goto done;
    }
    flock(fd, LOCK_UN | LOCK_NB);
    LOG_PRINT(LOG_DEBUG, "Image [%s] Write Successfully!", save_name);
    result = 1;

done:
    if (fd != -1)
        close(fd);

    return result;
}

void save_timg(char * filePath, mp_arg_t *p) {
    zimg_req_t *zimg_req = NULL;
    zimg_req = (zimg_req_t *)malloc(sizeof(zimg_req_t));
    if (zimg_req == NULL) {
        LOG_PRINT(LOG_DEBUG, "zimg_req malloc failed!");
        return;
    }
    zimg_req -> md5 = filePath;
    zimg_req -> type = NULL;
    zimg_req -> width = 0;
    zimg_req -> height = 0;
    zimg_req -> proportion = 1;
    zimg_req -> gray = 0;
    zimg_req -> x = -1;
    zimg_req -> y = -1;
    zimg_req -> rotate = 0;
    zimg_req -> quality = settings.quality;
    zimg_req -> fmt = settings.format;
    zimg_req -> sv = 0;
    zimg_req -> thr_arg = p->thr_arg;

    get_timg(zimg_req,p);

	if (p->width>0 && p->width<5000)
	{
		//LOG_PRINT(LOG_DEBUG, "zimg_req width %d",p->width);
		zimg_req->width = p->width;
		get_timg(zimg_req, p);
	}

    free(zimg_req);
}

int get_timg(zimg_req_t *req, mp_arg_t *p) {
	int result = -1;
	char rsp_cache_key[CACHE_KEY_SIZE];
	int fd = -1;
	struct stat f_stat;
	char *buff = NULL;
	char *orig_buff = NULL;
	MagickWand *im = NULL;
	size_t len = 0;
	bool to_save = true;

	LOG_PRINT(LOG_DEBUG, "get_timg() start processing zimg request...");


	if (is_dir(req->md5) == -1) {
		LOG_PRINT(LOG_DEBUG, "Image %s is not existed!", req->md5);
		goto err;
	}

	LOG_PRINT(LOG_DEBUG, "Start to Find the Image...");

	char orig_path[512];
	snprintf(orig_path, 512, "%s/src/%s", req->md5,p->filename);
	LOG_PRINT(LOG_DEBUG, "0rig File Path: %s", orig_path);

	char rsp_path[512];
	if (req->width==0)
	{
		snprintf(rsp_path, 512, "%s/%s", req->md5, p->filename);
		LOG_PRINT(LOG_DEBUG, "Got the rsp_path: %s", rsp_path);
	}
	else {
		snprintf(rsp_path, 512, "%s/%d/", req->md5,req->width);
		if (is_dir(rsp_path) != 1) {
			if (mk_dirs(rsp_path) == -1) {
				LOG_PRINT(LOG_DEBUG, "save_path[%s] Create Failed!", rsp_path);
				return;
			}
			LOG_PRINT(LOG_DEBUG, "rsp_path[%s] Create Finish.", rsp_path);
		}
		snprintf(rsp_path, 512, "%s/%d/%s", req->md5, req->width,p->filename);
		LOG_PRINT(LOG_DEBUG, "Got the rsp_path: %s", rsp_path);
	}
	

	if ((fd = open(rsp_path, O_RDONLY)) == -1) {
		im = NewMagickWand();
		if (im == NULL) goto err;

		int ret;
	
		ret = MagickReadImage(im, orig_path);
		if (ret != MagickTrue) {
			LOG_PRINT(LOG_DEBUG, "Open Original Image From Disk Failed! %d != %d", ret, MagickTrue);
			LOG_PRINT(LOG_DEBUG, "Open Original Image From Disk Failed!");
			goto err;
		}
		else {
			MagickSizeType size;
			MagickGetImageLength(im, &size);
			LOG_PRINT(LOG_DEBUG, "image size = %d", size);
			if (size < CACHE_MAX_SIZE) {
				MagickResetIterator(im);
				char *new_buff = (char *)MagickGetImageBlob(im, &len);
				if (new_buff == NULL) {
					LOG_PRINT(LOG_DEBUG, "Webimg Get Original Blob Failed!");
					goto err;
				}
				free(new_buff);
			}
		}
		
		if (settings.script_on == 1 && req->type != NULL)
			ret = lua_convert(im, req);
		else
			ret = convert(im, req);
		if (ret == -1) goto err;
		if (ret == 0) to_save = false;

		buff = (char *)MagickGetImageBlob(im, &len);
		if (buff == NULL) {
			LOG_PRINT(LOG_DEBUG, "Webimg Get Blob Failed!");
			goto err;
		}
	}
	else {
		to_save = false;
		fstat(fd, &f_stat);
		size_t rlen = 0;
		len = f_stat.st_size;
		if (len <= 0) {
			LOG_PRINT(LOG_DEBUG, "File[%s] is Empty.", rsp_path);
			goto err;
		}
		if ((buff = (char *)malloc(len)) == NULL) {
			LOG_PRINT(LOG_DEBUG, "buff Malloc Failed!");
			goto err;
		}
		LOG_PRINT(LOG_DEBUG, "img_size = %d", len);
		if ((rlen = read(fd, buff, len)) == -1) {
			LOG_PRINT(LOG_DEBUG, "File[%s] Read Failed: %s", rsp_path, strerror(errno));
			goto err;
		}
		else if (rlen < len) {
			LOG_PRINT(LOG_DEBUG, "File[%s] Read Not Compeletly.", rsp_path);
			goto err;
		}
	}

	//LOG_PRINT(LOG_INFO, "New Image[%s]", rsp_path);
	int save_new = 0;
	if (to_save == true) {
		if (req->sv == 1 || settings.save_new == 1 || (settings.save_new == 2 && req->type != NULL)) {
			save_new = 1;
		}
	}

	if (save_new == 1) {
		LOG_PRINT(LOG_DEBUG, "Image[%s] is Not Existed. Begin to Save it.", rsp_path);
		if (new_img(buff, len, rsp_path) == -1) {
			LOG_PRINT(LOG_DEBUG, "New Image[%s] Save Failed!", rsp_path);
			LOG_PRINT(LOG_WARNING, "fail save %s", rsp_path);
		}
	}
	else
		LOG_PRINT(LOG_DEBUG, "Image [%s] Needn't to Storage.", rsp_path);


// done:
// 	if (request != NULL) {
// 		if (settings.etag == 1) {
// 			result = zimg_etag_set(request, buff, len);
// 			if (result == 2)
// 				goto err;
// 		}
// 		result = evbuffer_add(request->buffer_out, buff, len);
// 	}
// 	if (result != -1) {
// 		result = 1;
// 	}

err:
	if (fd != -1)
		close(fd);
	if (im != NULL)
		DestroyMagickWand(im);
	free(buff);
	return result;
}

/**
 * @brief get_img get image from disk mode through the request
 *
 * @param req the zimg request
 * @param request the evhtp request
 *
 * @return 1 for OK, 2 for 304 needn't response buffer and -1 for failed
 */
int get_img(zimg_req_t *req, evhtp_request_t *request) {
    int result = -1;
    char rsp_cache_key[CACHE_KEY_SIZE];
    int fd = -1;
    struct stat f_stat;
    char *buff = NULL;
    char *orig_buff = NULL;
    MagickWand *im = NULL;
    size_t len = 0;
    bool to_save = true;

    LOG_PRINT(LOG_DEBUG, "get_img() start processing zimg request...");

    int lvl1 = str_hash(req->md5);
    int lvl2 = str_hash(req->md5 + 3);

    char whole_path[512];
    snprintf(whole_path, 512, "%s/%d/%d/%s", settings.old_path, lvl1, lvl2, req->md5);
    LOG_PRINT(LOG_DEBUG, "whole_path: %s", whole_path);

    if (is_dir(whole_path) == -1) {
        LOG_PRINT(LOG_DEBUG, "Image %s is not existed!", req->md5);
        goto err;
    }

    if (settings.script_on == 1 && req->type != NULL)
        snprintf(rsp_cache_key, CACHE_KEY_SIZE, "%s:%s", req->md5, req->type);
    else {
        if (req->proportion == 0 && req->width == 0 && req->height == 0)
            str_lcpy(rsp_cache_key, req->md5, CACHE_KEY_SIZE);
        else
            gen_key(rsp_cache_key, req->md5, 9, req->width, req->height, req->proportion, req->gray, req->x, req->y, req->rotate, req->quality, req->fmt);
    }

    if (find_cache_bin(req->thr_arg, rsp_cache_key, &buff, &len) == 1) {
        LOG_PRINT(LOG_DEBUG, "Hit Cache[Key: %s].", rsp_cache_key);
        to_save = false;
        goto done;
    }
    LOG_PRINT(LOG_DEBUG, "Start to Find the Image...");

    char orig_path[512];
    snprintf(orig_path, 512, "%s/0*0", whole_path);
    LOG_PRINT(LOG_DEBUG, "0rig File Path: %s", orig_path);

    char rsp_path[512];
    if (settings.script_on == 1 && req->type != NULL)
        snprintf(rsp_path, 512, "%s/t_%s", whole_path, req->type);
    else {
        char name[128];
        snprintf(name, 128, "%d*%d_p%d_g%d_%d*%d_r%d_q%d.%s", req->width, req->height,
                 req->proportion,
                 req->gray,
                 req->x, req->y,
                 req->rotate,
                 req->quality,
                 req->fmt);

        if (req->width == 0 && req->height == 0 && req->proportion == 0) {
            LOG_PRINT(LOG_DEBUG, "Return original image.");
            strncpy(rsp_path, orig_path, 512);
        } else {
            snprintf(rsp_path, 512, "%s/%s", whole_path, name);
        }
    }
    LOG_PRINT(LOG_DEBUG, "Got the rsp_path: %s", rsp_path);

    if ((fd = open(rsp_path, O_RDONLY)) == -1) {
        im = NewMagickWand();
        if (im == NULL) goto err;

        int ret;
        if (find_cache_bin(req->thr_arg, req->md5, &orig_buff, &len) == 1) {
            LOG_PRINT(LOG_DEBUG, "Hit Orignal Image Cache[Key: %s].", req->md5);

            ret = MagickReadImageBlob(im, (const unsigned char *)orig_buff, len);
            if (ret != MagickTrue) {
                LOG_PRINT(LOG_DEBUG, "Open Original Image From Blob Failed! Begin to Open it From Disk.");
                del_cache(req->thr_arg, req->md5);
                ret = MagickReadImage(im, orig_path);
                if (ret != MagickTrue) {
                    LOG_PRINT(LOG_DEBUG, "Open Original Image From Disk Failed!");
                    goto err;
                } else {
                    MagickSizeType size;
                    MagickGetImageLength(im, &size);
                    LOG_PRINT(LOG_DEBUG, "image size = %d", size);
                    if (size < CACHE_MAX_SIZE) {
                        MagickResetIterator(im);
                        char *new_buff = (char *)MagickGetImageBlob(im, &len);
                        if (new_buff == NULL) {
                            LOG_PRINT(LOG_DEBUG, "Webimg Get Original Blob Failed!");
                            goto err;
                        }
                        set_cache_bin(req->thr_arg, req->md5, new_buff, len);
                        free(new_buff);
                    }
                }
            }
        } else {
            LOG_PRINT(LOG_DEBUG, "Not Hit Original Image Cache. Begin to Open it.");
            ret = MagickReadImage(im, orig_path);
            if (ret != MagickTrue) {
                LOG_PRINT(LOG_DEBUG, "Open Original Image From Disk Failed! %d != %d", ret, MagickTrue);
                LOG_PRINT(LOG_DEBUG, "Open Original Image From Disk Failed!");
                goto err;
            } else {
                MagickSizeType size;
                MagickGetImageLength(im, &size);
                LOG_PRINT(LOG_DEBUG, "image size = %d", size);
                if (size < CACHE_MAX_SIZE) {
                    MagickResetIterator(im);
                    char *new_buff = (char *)MagickGetImageBlob(im, &len);
                    if (new_buff == NULL) {
                        LOG_PRINT(LOG_DEBUG, "Webimg Get Original Blob Failed!");
                        goto err;
                    }
                    set_cache_bin(req->thr_arg, req->md5, new_buff, len);
                    free(new_buff);
                }
            }
        }

        if (settings.script_on == 1 && req->type != NULL)
            ret = lua_convert(im, req);
        else
            ret = convert(im, req);
        if (ret == -1) goto err;
        if (ret == 0) to_save = false;

        buff = (char *)MagickGetImageBlob(im, &len);
        if (buff == NULL) {
            LOG_PRINT(LOG_DEBUG, "Webimg Get Blob Failed!");
            goto err;
        }
    } else {
        to_save = false;
        fstat(fd, &f_stat);
        size_t rlen = 0;
        len = f_stat.st_size;
        if (len <= 0) {
            LOG_PRINT(LOG_DEBUG, "File[%s] is Empty.", rsp_path);
            goto err;
        }
        if ((buff = (char *)malloc(len)) == NULL) {
            LOG_PRINT(LOG_DEBUG, "buff Malloc Failed!");
            goto err;
        }
        LOG_PRINT(LOG_DEBUG, "img_size = %d", len);
        if ((rlen = read(fd, buff, len)) == -1) {
            LOG_PRINT(LOG_DEBUG, "File[%s] Read Failed: %s", rsp_path, strerror(errno));
            goto err;
        } else if (rlen < len) {
            LOG_PRINT(LOG_DEBUG, "File[%s] Read Not Compeletly.", rsp_path);
            goto err;
        }
    }

    //LOG_PRINT(LOG_INFO, "New Image[%s]", rsp_path);
    int save_new = 0;
    if (to_save == true) {
        if (req->sv == 1 || settings.save_new == 1 || (settings.save_new == 2 && req->type != NULL)) {
            save_new = 1;
        }
    }

    if (save_new == 1) {
        LOG_PRINT(LOG_DEBUG, "Image[%s] is Not Existed. Begin to Save it.", rsp_path);
        if (new_img(buff, len, rsp_path) == -1) {
            LOG_PRINT(LOG_DEBUG, "New Image[%s] Save Failed!", rsp_path);
            LOG_PRINT(LOG_WARNING, "fail save %s", rsp_path);
        }
    } else
        LOG_PRINT(LOG_DEBUG, "Image [%s] Needn't to Storage.", rsp_path);

    if (len < CACHE_MAX_SIZE) {
        set_cache_bin(req->thr_arg, rsp_cache_key, buff, len);
    }

done:
    if (request != NULL) {
        if (settings.etag == 1) {
            result = zimg_etag_set(request, buff, len);
            if (result == 2)
                goto err;
        }
        result = evbuffer_add(request->buffer_out, buff, len);
    }
    if (result != -1) {
        result = 1;
    }

err:
    if (fd != -1)
        close(fd);
    if (im != NULL)
        DestroyMagickWand(im);
    free(buff);
    free(orig_buff);
    return result;
}

/**
 * @brief admin_img the function to deal with admin reqeust for disk mode
 *
 * @param req the evhtp request
 * @param md5 the file md5
 * @param t admin type
 *
 * @return 1 for OK, 2 for 404 not found and -1 for fail
 */
int admin_img(evhtp_request_t *req, thr_arg_t *thr_arg, char *md5, int t) {
    int result = -1;

    LOG_PRINT(LOG_DEBUG, "amdin_img() start processing admin request...");
    char whole_path[512];
    int lvl1 = str_hash(md5);
    int lvl2 = str_hash(md5 + 3);
    snprintf(whole_path, 512, "%s/%d/%d/%s", settings.img_path, lvl1, lvl2, md5);
    LOG_PRINT(LOG_DEBUG, "whole_path: %s", whole_path);

    if (is_dir(whole_path) == -1) {
        LOG_PRINT(LOG_DEBUG, "path: %s is not exist!", whole_path);
        return 2;
    }

    if (t == 1) {
        if (delete_file(whole_path) != -1) {
            result = 1;
            evbuffer_add_printf(req->buffer_out,
                                "<html><body><h1>Admin Command Successful!</h1> \
                <p>MD5: %s</p> \
                <p>Command Type: %d</p> \
                </body></html>",
                                md5, t);
            evhtp_headers_add_header(req->headers_out, evhtp_header_new("Content-Type", "text/html", 0, 0));
        }
    }
    return result;
}

/**
 * @brief info_img the function of getting info of a image
 *
 * @param md5 the md5 of image
 * @param request the evhtp request
 *
 * @return 1 for OK and -1 for fail
 */
int info_img(evhtp_request_t *request, thr_arg_t *thr_arg, char *md5) {
    int result = -1;

    LOG_PRINT(LOG_DEBUG, "info_img() start processing info request...");
    MagickWand *im = NULL;
    char whole_path[512];
    int lvl1 = str_hash(md5);
    int lvl2 = str_hash(md5 + 3);
    snprintf(whole_path, 512, "%s/%d/%d/%s", settings.img_path, lvl1, lvl2, md5);
    LOG_PRINT(LOG_DEBUG, "whole_path: %s", whole_path);

    if (is_dir(whole_path) == -1) {
        result = 0;
        LOG_PRINT(LOG_DEBUG, "Image %s is not existed!", md5);
        goto err;
    }

    char orig_path[512];
    snprintf(orig_path, 512, "%s/0*0", whole_path);
    LOG_PRINT(LOG_DEBUG, "0rig File Path: %s", orig_path);

    im = NewMagickWand();
    if (im == NULL) goto err;
    int ret = -1;

    ret = MagickReadImage(im, orig_path);
    if (ret != MagickTrue) {
        LOG_PRINT(LOG_DEBUG, "Open Original Image From Disk Failed!");
        goto err;
    }

    add_info(im, request);
    result = 1;

err:
    if (im != NULL)
        DestroyMagickWand(im);
    return result;
}
