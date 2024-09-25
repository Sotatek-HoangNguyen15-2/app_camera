#ifndef __UTILS_H
#define __UTILS_H

#ifdef  __cplusplus
extern "C" {
#endif

int
os_create_anonymous_file(off_t size);

const char*
get_first_camera_device(void);

#ifdef  __cplusplus
}
#endif

#endif
