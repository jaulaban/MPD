/*
 * Copyright 2003-2016 The Music Player Daemon Project
 * http://www.musicpd.org
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#ifndef MPD_GLOBAL_EVENTS_HXX
#define MPD_GLOBAL_EVENTS_HXX

#include "event/MaskMonitor.hxx"

namespace GlobalEvents {
	enum Event {
		/** must call playlist_sync() */
		PLAYLIST,

		/** the current song's tag has changed */
		TAG,

		MAX
	};

	typedef void (*Handler)();

	class Monitor final : MaskMonitor {
		Handler handlers[MAX];

	public:
		explicit Monitor(EventLoop &_loop):MaskMonitor(_loop) {}

		void Register(Event event, Handler handler);

		void Emit(Event event);

	private:
		/**
		 * Invoke the callback for a certain event.
		 */
		void Invoke(Event event);

	protected:
		void HandleMask(unsigned mask) override;
	};
}

#endif /* MAIN_NOTIFY_H */
