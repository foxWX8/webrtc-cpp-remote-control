#include "pch.h"
#include "InputInjectorWin.h"

namespace
{
	constexpr ULONG_PTR kRemoteInputMarker = static_cast<ULONG_PTR>(0x52435431u); // RTC1
	bool Send(INPUT& input) { return ::SendInput(1, &input, sizeof(input)) == 1; }

	void FillAbsoluteMouseMove(INPUT& input, unsigned short x, unsigned short y)
	{
		const int virtual_left = ::GetSystemMetrics(SM_XVIRTUALSCREEN);
		const int virtual_top = ::GetSystemMetrics(SM_YVIRTUALSCREEN);
		const int virtual_width = (::GetSystemMetrics(SM_CXVIRTUALSCREEN) > 1) ?
			::GetSystemMetrics(SM_CXVIRTUALSCREEN) : 2;
		const int virtual_height = (::GetSystemMetrics(SM_CYVIRTUALSCREEN) > 1) ?
			::GetSystemMetrics(SM_CYVIRTUALSCREEN) : 2;
		const int primary_width = (::GetSystemMetrics(SM_CXSCREEN) > 1) ? ::GetSystemMetrics(SM_CXSCREEN) : 2;
		const int primary_height = (::GetSystemMetrics(SM_CYSCREEN) > 1) ? ::GetSystemMetrics(SM_CYSCREEN) : 2;
		const int primary_x = ::MulDiv(x, primary_width - 1, 65535);
		const int primary_y = ::MulDiv(y, primary_height - 1, 65535);
		const int absolute_x = (std::max)(0, (std::min)(65535,
			::MulDiv(primary_x - virtual_left, 65535, virtual_width - 1)));
		const int absolute_y = (std::max)(0, (std::min)(65535,
			::MulDiv(primary_y - virtual_top, 65535, virtual_height - 1)));

		input = {};
		input.type = INPUT_MOUSE;
		input.mi.dx = absolute_x;
		input.mi.dy = absolute_y;
		input.mi.dwExtraInfo = kRemoteInputMarker;
		input.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
	}

	bool FillMouseButton(INPUT& input, unsigned char button, bool down)
	{
		static const DWORD flags[][2] = {{MOUSEEVENTF_LEFTUP, MOUSEEVENTF_LEFTDOWN},
			{MOUSEEVENTF_RIGHTUP, MOUSEEVENTF_RIGHTDOWN},
			{MOUSEEVENTF_MIDDLEUP, MOUSEEVENTF_MIDDLEDOWN}};
		if (button > 2) return false;
		input = {};
		input.type = INPUT_MOUSE;
		input.mi.dwFlags = flags[button][down ? 1 : 0];
		input.mi.dwExtraInfo = kRemoteInputMarker;
		return true;
	}
}

bool InputInjectorWin::IsInjectedMessage()
{
	return static_cast<ULONG_PTR>(::GetMessageExtraInfo()) == kRemoteInputMarker;
}

bool InputInjectorWin::MouseMove(unsigned short x, unsigned short y)
{
	// DXGI captures the primary monitor, while MOUSEEVENTF_VIRTUALDESK expects
	// coordinates across the entire virtual desktop. Convert explicitly so a
	// multi-monitor receiver still maps 0..65535 to its primary screen.
	INPUT input{};
	FillAbsoluteMouseMove(input, x, y);
	return Send(input);
}

bool InputInjectorWin::MouseButton(unsigned char button, bool down)
{
	INPUT input{};
	if (!FillMouseButton(input, button, down)) return false;
	return Send(input);
}

bool InputInjectorWin::MouseButtonAt(unsigned short x, unsigned short y, unsigned char button, bool down)
{
	INPUT inputs[2]{};
	FillAbsoluteMouseMove(inputs[0], x, y);
	if (!FillMouseButton(inputs[1], button, down)) return false;
	// A single SendInput call guarantees that no unrelated local input can be
	// interleaved between positioning the cursor and applying the button state.
	return ::SendInput(2, inputs, sizeof(INPUT)) == 2;
}

bool InputInjectorWin::MouseWheel(short delta)
{
	INPUT input{}; input.type = INPUT_MOUSE; input.mi.dwFlags = MOUSEEVENTF_WHEEL; input.mi.mouseData = static_cast<DWORD>(delta);
	input.mi.dwExtraInfo = kRemoteInputMarker;
	return Send(input);
}

bool InputInjectorWin::Key(unsigned short key, bool down)
{
	INPUT input{}; input.type = INPUT_KEYBOARD; input.ki.wVk = key; input.ki.dwFlags = down ? 0 : KEYEVENTF_KEYUP;
	input.ki.dwExtraInfo = kRemoteInputMarker;
	return Send(input);
}
