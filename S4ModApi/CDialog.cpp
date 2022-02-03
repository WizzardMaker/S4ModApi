///////////////////////////////////////////////////////////////////////////////
// GNU Lesser General Public License v3 (LGPL v3) 
//
// Copyright (c) 2022 nyfrk <nyfrk@gmx.net> and contributors
//
// This file is part of S4ModApi.
//
// S4ModApi is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// S4ModApi is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with S4ModApi. If not, see <https://www.gnu.org/licenses/lgpl-3.0>.
///////////////////////////////////////////////////////////////////////////////

#include "CDialog.h"
#include <algorithm>
#include "CFrameHook.h"
#include "CMouseHook.h"

#include "s4.h" // pillar box width

#define IncrementFeatureCounts() ModifyFeatureCounts(1)
#define DecrementFeatureCounts() ModifyFeatureCounts(-1)

std::mutex CDialog::state_mutex;
std::vector<CDialog*> CDialog::dialogs; // how many dialogs are visible
S4API CDialog::s4api;
S4HOOK CDialog::hFramehook, CDialog::hMousehook;
std::vector<CDialog*> CDialog::pending_dialogs; // how many dialogs want to be visible
volatile CDialog::State CDialog::state;
std::condition_variable CDialog::condIdle; // wait for various states
unsigned CDialog::countFramehook = 0, CDialog::countMousehook = 0;

CDialog::CDialog(INT x, INT y, INT w, INT h, DWORD flags, FeaturesEnum f) :
	m_position({ x, y, x + w, y + h }),
	m_flags(flags),
	m_isShown(false),
	DialogFeatures(f) {	TRACE; }

CDialog::CDialog(DWORD flags, FeaturesEnum f) :
	m_position({ 200, 200, 300, 300 }),
	m_isShown(false),
	m_flags(flags),
	DialogFeatures(f) {
	TRACE;
}

const RECT& CDialog::GetRect() const {
	TRACE;
	return m_position;
}

void CDialog::MaintainS4Api() {
	TRACE;
	if (s4api) {
		if (hFramehook && countFramehook <= 0) {
			s4api->RemoveListener(hFramehook);
			hFramehook = 0;
		}
		if (hMousehook && countMousehook <= 0) {
			s4api->RemoveListener(hMousehook);
			hMousehook = 0;
		}
		if (dialogs.empty()) {
			s4api->Release();
			s4api = NULL;
		}
	}
	else if (!dialogs.empty()) {
		s4api = S4ApiCreate();
		hFramehook = 0;
		hMousehook = 0;
	}
	if (s4api) {
		if (!hFramehook && countFramehook > 0) hFramehook = CFrameHook::GetInstance().AddListener(OnFrameProc,0, DIALOG_RENDER_PRIORITY);
		if (!hMousehook && countMousehook > 0) hMousehook = CMouseHook::GetInstance().AddListener(OnMouseProc,0, DIALOG_RENDER_PRIORITY);
	}
}

void CDialog::ModifyFeatureCounts(signed add) {
	TRACE;
	if (HasFeature(FeatureOnDraw)) countFramehook += add;
	if (HasFeature(FeatureOnMouse)) countMousehook += add;
}

BOOL CDialog::Show() {
	TRACE;
	std::lock_guard<decltype(state_mutex)> lock(state_mutex);
	if (m_isShown) return FALSE;
	m_isShown = true;
	if (!OnShow()) {
		return FALSE;
	}
	IncrementFeatureCounts();
	switch (state) {
		case StateIdle:
			dialogs.push_back(this);
			MaintainS4Api();
			break;
		case StateBusy:
		case StateBusyCleanupRequired:
			pending_dialogs.push_back(this);
			state = StateBusyCleanupRequired;
			break;
	}
	return TRUE;
}

BOOL CDialog::Hide() {
	TRACE;
	std::lock_guard<decltype(state_mutex)> lock(state_mutex);
	if (!m_isShown) return FALSE;
	m_isShown = false;
	if (!OnHide()) {
		return FALSE;
	}
	DecrementFeatureCounts();
	switch (state) {
	case StateIdle: {
			auto it = std::find(dialogs.rbegin(), dialogs.rend(), this);
			if (it != dialogs.rend()) {
				dialogs.erase((++it).base());
			}
			MaintainS4Api();
			break;
		}
		case StateBusyCleanupRequired: {
			auto it = std::find(pending_dialogs.rbegin(), pending_dialogs.rend(), this);
			if (it != pending_dialogs.rend()) {
				pending_dialogs.erase((++it).base());
				goto BusyCleanupRequiredEndOfSwitch;
			}
		} // didnt find, try in dialogs instead (not break out here)
		case StateBusy: {
			// mutex not required here, because state_mutex is still locked
			auto it = std::find(dialogs.rbegin(), dialogs.rend(), this);
			if (it != dialogs.rend()) {
				*it = NULL;
			}
			state = StateBusyCleanupRequired;
			break;
		}
	}
	BusyCleanupRequiredEndOfSwitch:
	return TRUE;
}

BOOL CDialog::HasFeature(const FeaturesEnum& f) const {
	TRACE;
	return (DialogFeatures & f) == f;
}

BOOL CDialog::IsShown() const {
	TRACE;
	std::lock_guard<decltype(state_mutex)> lock(state_mutex);
	return m_isShown;
}

CDialog::~CDialog() {
	TRACE;
	Hide();
}

BOOL CDialog::OnDraw(HDC hdc, const POINT* cursor, const RECT* clientRect) {
	TRACE;
	UNREFERENCED_PARAMETER(hdc);
	UNREFERENCED_PARAMETER(cursor);
	UNREFERENCED_PARAMETER(clientRect);
	return FALSE; // nothing drawn
}

BOOL CDialog::OnMouse(DWORD dwMouseButton, INT iX, INT iY, DWORD dwMsgId, HWND hwnd) {
	TRACE;
	UNREFERENCED_PARAMETER(dwMouseButton);
	UNREFERENCED_PARAMETER(iX);
	UNREFERENCED_PARAMETER(iY);
	UNREFERENCED_PARAMETER(dwMsgId);
	UNREFERENCED_PARAMETER(hwnd);
	return FALSE; // not consume
}

BOOL CDialog::OnShow() {
	TRACE;
	return TRUE; // allow show
}

BOOL CDialog::OnHide() {
	TRACE;
	return TRUE; // allow hide
}

void CDialog::Cleanup() {
	TRACE;
	if (state == StateBusyCleanupRequired) {
		// erase all nullpointers
		dialogs.erase(std::remove(dialogs.begin(), dialogs.end(), (CDialog*)NULL), dialogs.end());
		// insert all pending dialogs
		dialogs.insert(std::end(dialogs), std::begin(pending_dialogs), std::end(pending_dialogs));
		pending_dialogs.clear();
		MaintainS4Api();
	}
	state = StateIdle;
	condIdle.notify_one();
}

HRESULT S4HCALL CDialog::OnFrameProc(LPDIRECTDRAWSURFACE7 lpSurface, INT32 iPillarboxWidth, LPVOID lpReserved) {
	TRACE;
	UNREFERENCED_PARAMETER(iPillarboxWidth);
	UNREFERENCED_PARAMETER(lpReserved);
	std::unique_lock<decltype(state_mutex)> lock(state_mutex);
	POINT p = { 0 };
	RECT clientRect = { 0 };
	HWND hwnd = s4api ? s4api->GetHwnd() : NULL;
	const POINT* pp = (hwnd && GetCursorPos(&p) && ScreenToClient(hwnd, &p)) ? &p : NULL;
	if (hwnd) GetClientRect(hwnd, &clientRect);
	HDC hdc;
	lpSurface->GetDC(&hdc);
	while (state != StateIdle) {
		condIdle.wait(lock);
	}
	state = StateBusy; // the vector cannot change in size now, which should make it thread safe
	for (auto inst : dialogs) { // do not make it a ref, since it may change as soon as we unlock
		if (inst == NULL) {
			state = StateBusyCleanupRequired; 
			continue;
		}
		lock.unlock();
		inst->OnDraw(hdc, pp, &clientRect);
		lock.lock();
	}
	lpSurface->ReleaseDC(hdc);
	Cleanup(); // in case some dialogs were added or removed during the callbacks, only call when state locked. Will set state back to idle.
	return 0;
}

HRESULT S4HCALL CDialog::OnMouseProc(DWORD dwMouseButton, INT iX, INT iY, DWORD dwMsgId, HWND hwnd, LPCS4UIELEMENT lpUiElement) {
	TRACE;
	UNREFERENCED_PARAMETER(dwMouseButton);
	UNREFERENCED_PARAMETER(hwnd);
	UNREFERENCED_PARAMETER(lpUiElement);

	BOOL consumeEvent = FALSE;

	switch (dwMsgId) {
		case WM_LBUTTONUP:
		case WM_LBUTTONDOWN:
		case WM_RBUTTONDOWN:
		case WM_RBUTTONUP: {
			POINT p;
			p.x = iX;
			p.y = iY;

			std::unique_lock<decltype(state_mutex)> lock(state_mutex);
			while (state != StateIdle) condIdle.wait(lock);
			state = StateBusy; // the vector cannot change in size now, which should make it thread safe

			// iterate backwards as we want to process the topmost window first
			for (auto it = dialogs.rbegin(); it != dialogs.rend(); ++it) {
				auto inst = *it;
				if (inst == NULL) {
					state = StateBusyCleanupRequired; // make sure we do a cleanup later
					continue;
				}
				if (!PtInRect(&inst->m_position, p)) { continue; } // no hit
				lock.unlock(); // prevent deadlock
				auto ret = inst->OnMouse(dwMouseButton, iX, iY, dwMsgId, hwnd);
				lock.lock();
				if (ret) {
					consumeEvent = TRUE;// window consumed the event
					break;
				} else {
					continue;
				}
			}
			Cleanup(); // in case some dialogs were added or removed during the callbacks, only call when state locked. Will set state back to idle.
		}
		default: break;
	}
	return consumeEvent;
}

VOID CDialog::UpdatePositionWithOffsetsFlags(const RECT& source, const RECT* clientRect) {
	m_position = source;
	RECT& rc = m_position;
	auto xoffset = 0, yoffset = 0;
	auto bitmapWidth = rc.right - rc.left;
	auto bitmapHeight = rc.bottom - rc.top;
	INT pillarboxWidth = 0;
	auto surfaceWidth = clientRect ? clientRect->right - clientRect->left : 0;
	auto surfaceHeight = clientRect ? clientRect->bottom - clientRect->top : 0;

	if ((m_flags & S4_CUSTOMUIFLAGS_NO_PILLARBOX) == 0) {
		auto pPillarboxWidth = S4::GetInstance().PillarboxWidth;
		if (pPillarboxWidth) pillarboxWidth = *pPillarboxWidth;
	}

	if (m_flags & S4_CUSTOMUIFLAGS_ANCHOR_CENTER) {
		xoffset -= bitmapWidth / 2;
	}
	else if (m_flags & S4_CUSTOMUIFLAGS_ANCHOR_RIGHT) {
		xoffset -= bitmapWidth;
	}
	if (m_flags & S4_CUSTOMUIFLAGS_ANCHOR_MIDDLE) {
		yoffset -= bitmapHeight / 2;
	}
	else if (m_flags & S4_CUSTOMUIFLAGS_ANCHOR_BOTTOM) {
		yoffset -= bitmapHeight;
	}
	if (m_flags & S4_CUSTOMUIFLAGS_ALIGN_CENTER) {
		xoffset += surfaceWidth / 2;
	}
	else if (m_flags & S4_CUSTOMUIFLAGS_ALIGN_RIGHT) {
		xoffset += surfaceWidth;
		xoffset -= pillarboxWidth;
	}
	else {
		xoffset += pillarboxWidth;
	}
	if (m_flags & S4_CUSTOMUIFLAGS_ALIGN_MIDDLE) {
		yoffset += surfaceHeight / 2;
	}
	else if (m_flags & S4_CUSTOMUIFLAGS_ALIGN_BOTTOM) {
		yoffset += surfaceHeight;
	}

	rc.left += xoffset;
	rc.right += xoffset;
	rc.top += yoffset;
	rc.bottom += yoffset;
}
