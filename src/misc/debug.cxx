// ----------------------------------------------------------------------------
//      debug.cxx
//
// Copyright (C) 2008
//              Stelios Bounanos, M0GLD
//
// This file is part of fldigi.
//
// fldigi is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 3 of the License, or
// (at your option) any later version.
//
// fldigi is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.
// ----------------------------------------------------------------------------

#include <config.h>

#ifdef __MINGW32__
#  include "compat.h"
#endif

#include <sstream>

#include <cstdio>
#include <cstring>
#include <cstdarg>

#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>

#include <FL/Fl.H>
#include <FL/Fl_Double_Window.H>
#include <FL/Fl_Slider.H>
#include <FL/Fl_Menu_Item.H>
#include <FL/Fl_Menu_Button.H>
#include "FTextView.h"

#include "debug.h"
#include "icons.h"
#include "gettext.h"

using namespace std;

#define MAX_LINES 65536

static FILE* wfile;
static FILE* rfile;
static size_t nlines = 0;
static int rfd;
static bool tty;
static bool want_popup = true;

static Fl_Double_Window* window;
static FTextView* text;

debug* debug::inst = 0;
debug::level_e debug::level = debug::WARN_LEVEL;
uint32_t debug::mask = ~0u;

const char* prefix[] = { _("Quiet"), _("Error"), _("Warning"), _("Info"), _("Debug") };

static void slider_cb(Fl_Widget* w, void*);
static void src_menu_cb(Fl_Widget* w, void*);
static void popup_message(void*);

Fl_Menu_Item src_menu[] = {
	{ _("Audio"), 0, 0, 0, FL_MENU_TOGGLE | FL_MENU_VALUE },
	{ _("Modem"), 0, 0, 0, FL_MENU_TOGGLE | FL_MENU_VALUE },
	{ _("Rig control"), 0, 0, 0, FL_MENU_TOGGLE | FL_MENU_VALUE },
	{ _("RPC"), 0, 0, 0, FL_MENU_TOGGLE | FL_MENU_VALUE },
	{ _("Spotter"), 0, 0, 0, FL_MENU_TOGGLE | FL_MENU_VALUE | FL_MENU_DIVIDER },
	{ _("Other"), 0, 0, 0, FL_MENU_TOGGLE | FL_MENU_VALUE },
	{ 0 }
};
#include <iostream>
void debug::rotate_log(const char* filename)
{
	const int n = 5; // rename existing log files to keep up to 5 old versions
	ostringstream oldfn, newfn;
	ostringstream::streampos p;

	oldfn << filename << '.';
	newfn << filename << '.';
	p = oldfn.tellp();

	for (int i = n - 1; i > 0; i--) {
		oldfn.seekp(p);
		newfn.seekp(p);
		oldfn << i;
		newfn << i + 1;
		rename(oldfn.str().c_str(), newfn.str().c_str());
	}
	rename(filename, oldfn.str().c_str());
}

void debug::start(const char* filename)
{
	if (debug::inst)
		return;
	rotate_log(filename);
	inst = new debug(filename);

	window = new Fl_Double_Window(512, 256, _("Event log"));
	window->xclass(PACKAGE_TARNAME);

	int pad = 2;
	Fl_Menu_Button* button = new Fl_Menu_Button(pad, pad, 128, 20, _("Log sources"));
	button->menu(src_menu);
	button->callback(src_menu_cb);

	Fl_Slider* slider = new Fl_Slider(button->x() + button->w() + pad, pad, 128, 20, prefix[level]);
	slider->tooltip(_("Change log level"));
	slider->align(FL_ALIGN_RIGHT);
	slider->type(FL_HOR_NICE_SLIDER);
	slider->range(0.0, LOG_NLEVELS - 1);
	slider->step(1.0);
	slider->value(level);
	slider->callback(slider_cb);

	text = new FTextView(pad, slider->h()+pad, window->w()-2*pad, window->h()-slider->h()-2*pad, 0);
	text->textfont(FL_COURIER);
	text->textsize(FL_NORMAL_SIZE);

	window->resizable(text);
	window->end();
}

void debug::stop(void)
{
	delete inst;
	inst = 0;
	delete window;
}

static char fmt[1024];

void debug::log(level_e level, const char* func, const char* srcf, int line, const char* format, ...)
{
	if (!inst)
		return;

	if (unlikely(debug::level == DEBUG_LEVEL)) {
		time_t t = time(NULL);
		struct tm stm;
		(void)localtime_r(&t, &stm);
		snprintf(fmt, sizeof(fmt), "%c: [%02d:%02d:%02d] %s:%d: %s\n",
			 *prefix[level], stm.tm_hour, stm.tm_min, stm.tm_sec, srcf, line, format);
	}
	else
		snprintf(fmt, sizeof(fmt), "%c: %s: %s\n", *prefix[level], func, format);
	va_list args;
	va_start(args, format);
	intptr_t nw = vfprintf(wfile, fmt, args);
	va_end(args);
	if (tty) {
		if (level <= DEBUG_LEVEL && level > QUIET_LEVEL) {
			va_start(args, format);
			vfprintf(stderr, fmt, args);
			va_end(args);
		}
	}
	else if (unlikely(want_popup && (level == ERROR_LEVEL || level == WARN_LEVEL))) {
		// If the backends logged an error and our stderr is not a tty,
		// alert the user and offer to show the log window.  We only do
		// this once and thereafter assume that the user will keep the
		// log window displayed to be notified of errors.  To keep the
		// popup small, we set a maximum width and add a ("...")  if it
		// must be truncated.
		const char ellipsis[] = "...";
		size_t len = 60;
		char* msg = new char[len + sizeof(ellipsis)];
		va_start(args, format);
		if ((size_t)vsnprintf(msg, len, fmt, args) >= len)
			memcpy(msg + len - 1, ellipsis, sizeof(ellipsis));
		va_end(args);
		Fl::add_timeout(0.0, popup_message, msg);
		want_popup = false;
	}

#ifdef __MINGW32__
	fflush(wfile);
#endif

	Fl::add_timeout(0.0, sync_text, (void*)nw);
}

void debug::elog(const char* func, const char* srcf, int line, const char* text)
{
	log(ERROR_LEVEL, func, srcf, line, "%s: %s", text, strerror(errno));
}

void debug::show(void)
{
	window->show();
}

static char buf[BUFSIZ+1];

void debug::sync_text(void* arg)
{
	intptr_t toread = (intptr_t)arg;
	size_t block = MIN((size_t)toread, sizeof(buf) - 1);
	ssize_t n;

	while (toread > 0) {
		if ((n = read(rfd, buf, block)) <= 0)
			break;
		if (unlikely(++nlines > MAX_LINES)) {
			text->clear();
			nlines = 0;
		}
		buf[n] = '\0';
		text->add(buf);
		toread -= n;
	}
}

debug::debug(const char* filename)
{
	if ((wfile = fopen(filename, "w")) == NULL)
		throw strerror(errno);
	setvbuf(wfile, (char*)NULL, _IOLBF, 0);
	set_cloexec(fileno(wfile), 1);

	if ((rfile = fopen(filename, "r")) == NULL)
		throw strerror(errno);
	rfd = fileno(rfile);
	set_cloexec(rfd, 1);
#ifndef __MINGW32__
	int f;
	if ((f = fcntl(rfd, F_GETFL)) == -1)
		throw strerror(errno);
	if (fcntl(rfd, F_SETFL, f | O_NONBLOCK) == -1)
		throw strerror(errno);
#endif
	tty = isatty(fileno(stderr));
}

debug::~debug()
{
	fclose(wfile);
	fclose(rfile);
}

static void slider_cb(Fl_Widget* w, void*)
{
	debug::level = (debug::level_e)((Fl_Slider*)w)->value();
	w->label(prefix[debug::level]);
	w->parent()->redraw();
}

static void src_menu_cb(Fl_Widget* w, void*)
{
	debug::mask ^= 1 << ((Fl_Menu_*)w)->value();
}

static void popup_message(void* msg)
{
	if (window->visible()) {
		delete [] (char*)msg;
		return;
	}
	if (!Fl::first_window() || !Fl::first_window()->visible()) // defer
		return Fl::add_timeout(0.5, popup_message, msg);

	if (fl_warn_choice2("%s:\n%s", _("Close"), _("View log"),
			    NULL, _("A message was logged"), (char*)msg)) {
		window->show();
		text->insert_position(text->buffer()->length());
		text->show_insert_position();
	}

	delete [] (char*)msg;
}
