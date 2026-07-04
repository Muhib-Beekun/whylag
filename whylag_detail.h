#ifndef WHYLAG_DETAIL_H
#define WHYLAG_DETAIL_H

#include <windows.h>

void whylag_show_text_dialog(HWND parent, const char *title, const char *body);
void whylag_show_item_detail(HWND parent, HWND list, int tab_index);

#endif
