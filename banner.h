#ifndef BANNER_H
#define BANNER_H

#ifdef __cplusplus
extern "C" {
#endif

void banner_print(const char *group, const char *course, const char *title);
void banner_print_menu(const char *const options[], unsigned int count);

#ifdef __cplusplus
}
#endif

#endif
