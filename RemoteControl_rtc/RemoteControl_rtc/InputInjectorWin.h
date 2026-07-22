#pragma once

class InputInjectorWin final
{
public:
	static bool IsInjectedMessage();
	static bool MouseMove(unsigned short normalized_x, unsigned short normalized_y);
	static bool MouseButton(unsigned char button, bool down);
	static bool MouseButtonAt(unsigned short normalized_x, unsigned short normalized_y,
		unsigned char button, bool down);
	static bool MouseWheel(short delta);
	static bool Key(unsigned short virtual_key, bool down);
};
