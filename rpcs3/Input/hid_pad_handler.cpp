#include "hid_pad_handler.h"
#include "ds3_pad_handler.h"
#include "ds4_pad_handler.h"
#include "dualsense_pad_handler.h"
#include "skateboard_pad_handler.h"
#include "ps_move_handler.h"
#include "util/logs.hpp"
#include "Utilities/Timer.h"
#include "Emu/System.h"
#include "pad_thread.h"

#if defined(__APPLE__)
#include "3rdparty/hidapi/hidapi/mac/hidapi_darwin.h"
#endif

#ifdef _WIN32
#include <Windows.h>
#include <setupapi.h>
#include <cctype>
#endif

#include <algorithm>
#include <cstdio>
#include <memory>
#include <string_view>
#include <vector>

LOG_CHANNEL(hid_log, "HID");

#ifdef ANDROID
std::vector<android_usb_device> g_android_usb_devices;
std::mutex g_android_usb_devices_mutex;
#endif

// Global mutex to allow "hid_enumerate()" and "hid_open_path()" are accessed by one thread at a time
// (e.g. thread running "process()" and thread running enumerate_devices()).
// It avoids the emulation crash in case the controller gets disconnected (e.g. due to inactivity)
std::mutex g_hid_mutex;

struct hid_instance
{
public:
	hid_instance() = default;
	~hid_instance()
	{
		std::lock_guard lock(m_hid_mutex);

		// Only exit HIDAPI once on exit. HIDAPI uses a global state internally...
		if (m_initialized)
		{
			hid_log.notice("Exiting HIDAPI...");

			if (hid_exit() != 0)
			{
				hid_log.error("hid_exit failed!");
			}
		}
	}

	static hid_instance& get_instance()
	{
		static hid_instance instance {};
		return instance;
	}

	bool initialize()
	{
		std::lock_guard lock(m_hid_mutex);

		// Only init HIDAPI once. HIDAPI uses a global state internally...
		if (m_initialized)
		{
			return true;
		}

		hid_log.notice("Initializing HIDAPI ...");

#if defined(__APPLE__)
		int error_code = 0;
		Emu.BlockingCallFromMainThread([&error_code]()
		{
			error_code = hid_init();
			hid_darwin_set_open_exclusive(0);
		}, false);
#else
		const int error_code = hid_init();
#endif
		if (error_code != 0)
		{
			hid_log.fatal("hid_init error %d: %s", error_code, hid_error(nullptr));
			return false;
		}

		hid_log.notice("HIDAPI initialized");
		m_initialized = true;
		return true;
	}

private:
	bool m_initialized = false;
	std::mutex m_hid_mutex;
};

hid_device* HidDevice::open()
{
	// Lock before calling "hid_open_path()"
	std::unique_lock lock(g_hid_mutex, std::defer_lock);

	if (!lock.try_lock())
	{
		// The enumeration thread is busy. If we lock and open the device, we might get input stutter on other devices.
		return nullptr;
	}

#ifdef ANDROID
	hidDevice = hid_libusb_wrap_sys_device(path, -1);
#elif defined(__APPLE__)
	Emu.BlockingCallFromMainThread([this]()
	{
		hidDevice = hid_open_path(path.c_str());
	}, false);
#else
	hidDevice = hid_open_path(path.data());
#endif

	return hidDevice;
}

void HidDevice::close(hid_device* dev)
{
	if (!dev) return;

#if defined(__APPLE__)
	Emu.BlockingCallFromMainThread([dev]()
	{
		if (dev)
		{
			hid_close(dev);
		}
	}, false);
#else
	hid_close(dev);
#endif
}

void HidDevice::close()
{
#if defined(__APPLE__)
	if (hidDevice)
	{
		Emu.BlockingCallFromMainThread([this]()
		{
			if (hidDevice)
			{
				hid_close(hidDevice);
				hidDevice = nullptr;
			}
		}, false);
	}
#else
	if (hidDevice)
	{
		hid_close(hidDevice);
		hidDevice = nullptr;
	}
#endif

#ifdef _WIN32
	if (bt_device)
	{
		hid_close(bt_device);
		bt_device = nullptr;
	}
#endif
}

template <class Device>
hid_pad_handler<Device>::hid_pad_handler(pad_handler type, std::vector<id_pair> ids)
    : PadHandlerBase(type), m_ids(std::move(ids))
{
};

template <class Device>
hid_pad_handler<Device>::~hid_pad_handler()
{
	// Join thread
	m_enumeration_thread.reset();

	// Lock before accessing any controller (e.g. just to close it with "close()")
	std::lock_guard lock(g_hid_mutex);

	for (auto& controller : m_controllers)
	{
		if (controller.second)
		{
			controller.second->close();
		}
	}
}

template <class Device>
bool hid_pad_handler<Device>::Init()
{
	// Init() is reachable from the GUI thread, the pad-settings input thread and the
	// pad threads. Without serialization two threads can both run the first-time setup
	// and mutate m_controllers concurrently.
	std::lock_guard devices_lock(m_devices_mutex);

	if (m_is_init)
		return true;

	if (!hid_instance::get_instance().initialize())
		return false;

	for (usz i = 1; i <= MAX_GAMEPADS; i++) // Controllers 1-n in GUI
	{
		m_controllers.emplace(m_name_string + std::to_string(i), std::make_shared<Device>());
	}

	// Never call hid_enumerate on the Init caller. Init is reachable from the GUI
	// thread (list_devices / get_hid_device / pad settings). On Windows, hid_enumerate
	// can block indefinitely on Bluetooth DualSense (duplicate pairings / flaky HID),
	// which freezes the whole UI as Not Responding. The enumerator thread below owns
	// enumeration; process() -> update_devices() binds pads when results arrive.
	m_is_init = true;

	m_enumeration_thread = std::make_unique<named_thread<std::function<void()>>>(fmt::format("%s Enumerator", m_type), [this]()
	{
		while (thread_ctrl::state() != thread_state::aborting)
		{
			// Enumerate whenever pads are enabled — not only while a game is running —
			// so the first pass (and pad settings) can bind without blocking Init.
			if (pad::g_enabled)
			{
				enumerate_devices();
			}

			thread_ctrl::wait_for(2'000'000);
		}
	});

	return true;
}

template <class Device>
void hid_pad_handler<Device>::process()
{
	update_devices();

	PadHandlerBase::process();
}

template <class Device>
std::vector<pad_list_entry> hid_pad_handler<Device>::list_devices()
{
	std::vector<pad_list_entry> pads_list;

	if (!Init())
		return pads_list;

	std::lock_guard devices_lock(m_devices_mutex);

	for (const auto& controller : m_controllers) // Controllers 1-n in GUI
	{
		pads_list.emplace_back(controller.first, false);
	}

	return pads_list;
}

template <class Device>
void hid_pad_handler<Device>::enumerate_devices()
{
	Timer timer;
	std::set<hid_enumerated_device_type> device_paths;
	std::map<hid_enumerated_device_type, std::wstring> serials;

#ifdef ANDROID
	{
		std::lock_guard lock(g_android_usb_devices_mutex);
		for (const android_usb_device& device : g_android_usb_devices)
		{
			const auto filter = [&](id_pair id)
			{
				return id.m_vid == device.vendorId && id.m_pid == device.productId;
			};

			if (std::find_if(m_ids.begin(), m_ids.end(), filter) != m_ids.end())
			{
				device_paths.insert(device.fd);
			}
		}
	}
#else
#ifdef _WIN32
	// Avoid hid_enumerate on Windows. It opens every HID interface (CreateFile +
	// HidD_GetAttributes) and can block forever on a wedged Bluetooth DualSense
	// node (duplicate pairing stuck in "Removing device", ghost audio endpoints).
	// SetupDi interface listing returns paths without opening devices; VID/PID are
	// matched from the path string (vid_XXXX/pid_YYYY or VID&XXXXXXXX_PID&YYYY).
	{
		static const GUID guid_devinterface_hid =
			{0x4d1e55b2, 0xf16f, 0x11cf, {0x88, 0xcb, 0x00, 0x11, 0x11, 0x00, 0x00, 0x30}};

		const auto path_has_vid_pid = [](std::string_view path, u16 vid, u16 pid) -> bool
		{
			char vid_hid[16]{}, pid_hid[16]{}, pid_amp[16]{}, vid_tail[8]{};
			std::snprintf(vid_hid, sizeof(vid_hid), "vid_%04x", vid);
			std::snprintf(pid_hid, sizeof(pid_hid), "pid_%04x", pid);
			std::snprintf(pid_amp, sizeof(pid_amp), "pid&%04x", pid);
			std::snprintf(vid_tail, sizeof(vid_tail), "%04x", vid);

			std::string lower(path);
			for (char& c : lower)
			{
				c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
			}

			const bool pid_ok = lower.find(pid_hid) != umax || lower.find(pid_amp) != umax;
			if (!pid_ok)
			{
				return false;
			}

			if (lower.find(vid_hid) != umax)
			{
				return true;
			}

			// Bluetooth HID paths use VID&0002054C (last 4 hex digits = vendor id).
			usz pos = 0;
			while ((pos = lower.find("vid&", pos)) != umax)
			{
				const usz hex_start = pos + 4;
				if (hex_start + 8 <= lower.size() && lower.compare(hex_start + 4, 4, vid_tail) == 0)
				{
					return true;
				}
				pos = hex_start;
			}

			return false;
		};

		HDEVINFO dev_info = SetupDiGetClassDevsW(&guid_devinterface_hid, nullptr, nullptr,
			DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
		if (dev_info != INVALID_HANDLE_VALUE)
		{
			SP_DEVICE_INTERFACE_DATA iface{};
			iface.cbSize = sizeof(iface);

			for (DWORD index = 0; SetupDiEnumDeviceInterfaces(dev_info, nullptr, &guid_devinterface_hid, index, &iface); index++)
			{
				DWORD needed = 0;
				SetupDiGetDeviceInterfaceDetailW(dev_info, &iface, nullptr, 0, &needed, nullptr);
				if (needed < sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W))
				{
					continue;
				}

				std::vector<u8> buffer(needed);
				auto* detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(buffer.data());
				detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);

				if (!SetupDiGetDeviceInterfaceDetailW(dev_info, &iface, detail, needed, nullptr, nullptr))
				{
					continue;
				}

				// Narrow path the same way hidapi does (UTF-16 device path -> UTF-8/ACP).
				const wchar_t* wpath = detail->DevicePath;
				const int nbytes = WideCharToMultiByte(CP_ACP, 0, wpath, -1, nullptr, 0, nullptr, nullptr);
				if (nbytes <= 1)
				{
					continue;
				}

				std::string path(static_cast<usz>(nbytes - 1), '\0');
				WideCharToMultiByte(CP_ACP, 0, wpath, -1, path.data(), nbytes, nullptr, nullptr);

				bool matched = false;
				for (const auto& [vid, pid] : m_ids)
				{
					if (path_has_vid_pid(path, vid, pid))
					{
						matched = true;
						break;
					}
				}

				if (!matched)
				{
					continue;
				}

				// Bluetooth device nodes (BTHENUM\...) are not usable HID open paths for
				// pad input and a wedged "Removing device" pairing can hang CreateFile /
				// hid_open_path forever. Prefer the HID class interface paths only.
				std::string lower = path;
				for (char& c : lower)
				{
					c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
				}
				if (lower.find("bthenum") != umax)
				{
					hid_log.trace("%s skipping BTHENUM path: %s", m_type, path);
					continue;
				}

				hid_log.trace("%s SetupDi path: %s", m_type, path);
				device_paths.insert(path);

				// Match hidapi-Windows behaviour: only record serials for PS Move Col01.
				if (m_type == pad_handler::move && path.find("&Col01#") != umax)
				{
					serials[std::move(path)] = {};
				}
			}

			SetupDiDestroyDeviceInfoList(dev_info);
		}
		else
		{
			hid_log.error("SetupDiGetClassDevs failed (%u); falling back to hid_enumerate", GetLastError());
			for (const auto& [vid, pid] : m_ids)
			{
				std::lock_guard lock(g_hid_mutex);
				hid_device_info* head = hid_enumerate(vid, pid);
				for (hid_device_info* dev_info = head; dev_info != nullptr; dev_info = dev_info->next)
				{
					if (!dev_info->path)
					{
						continue;
					}
					std::string path = dev_info->path;
					device_paths.insert(path);
					serials[std::move(path)] = dev_info->serial_number ? std::wstring(dev_info->serial_number) : std::wstring();
				}
				hid_free_enumeration(head);
			}
		}
	}
#else
	for (const auto& [vid, pid] : m_ids)
	{
		// Let's make sure hid_enumerate is only done one thread at a time
		std::lock_guard lock(g_hid_mutex);

#if defined(__APPLE__)
		Emu.BlockingCallFromMainThread([&]()
		{
#endif
		hid_device_info* head = hid_enumerate(vid, pid);
		for (hid_device_info* dev_info = head; dev_info != nullptr; dev_info = dev_info->next)
		{
			if (!dev_info->path)
			{
				hid_log.error("Skipping enumeration of device with empty path.");
				continue;
			}

			std::string path = dev_info->path;
			device_paths.insert(path);
			serials[std::move(path)] = dev_info->serial_number ? std::wstring(dev_info->serial_number) : std::wstring();
		}
		hid_free_enumeration(head);
#if defined(__APPLE__)
		}, false);
#endif
	}
#endif
#endif
	hid_log.notice("%s enumeration found %d devices (%f ms)", m_type, device_paths.size(), timer.GetElapsedTimeInMilliSec());

#ifdef _WIN32
	if (m_type == pad_handler::move)
	{
		// Windows enumerates 3 ps move devices: Col01, Col02, and Col03.
		// We use Col01 for data and Col02 for bluetooth.

		// Filter paths. We only want the Col01 paths.
		std::set<std::string> col01_paths;

		for (const std::string& path : device_paths)
		{
			hid_log.trace("Found ps move device: %s", path);

			if (path.find("&Col01#") != umax)
			{
				col01_paths.insert(path);
			}
		}

		device_paths = std::move(col01_paths);
	}
#endif

	std::lock_guard lock(m_enumeration_mutex);
	m_new_enumerated_devices = std::move(device_paths);
	m_new_enumerated_serials = std::move(serials);
}

template <class Device>
void hid_pad_handler<Device>::update_devices()
{
	// Replaces entries of m_controllers and walks the whole map. Must not overlap with
	// Init(), list_devices() or get_hid_device() on another thread.
	std::lock_guard devices_lock(m_devices_mutex);

	{
		std::lock_guard lock(m_enumeration_mutex);

		if (m_enumerated_devices == m_new_enumerated_devices)
		{
			return;
		}

		m_enumerated_devices = m_new_enumerated_devices;
		m_enumerated_serials = std::move(m_new_enumerated_serials);
	}

	// Lock before accessing any controller (e.g. just to close it with "close()") or before calling "hid_open_path()"
	std::lock_guard lock(g_hid_mutex);

	// Scrap devices that are not in the new list
	for (auto& controller : m_controllers)
	{
		if (controller.second && controller.second->path != hid_enumerated_device_default && !m_enumerated_devices.contains(controller.second->path))
		{
			controller.second->close();
			cfg_pad* config = controller.second->config;
			controller.second.reset(new Device());
			controller.second->config = config;
		}
	}

	bool warn_about_drivers = false;

	// Find and add new devices
	for (const auto& path : m_enumerated_devices)
	{
		// Check if we have at least one virtual controller left
		if (std::none_of(m_controllers.cbegin(), m_controllers.cend(), [](const auto& c) { return !c.second || !c.second->hidDevice; }))
			break;

		// Check if we already have this controller
		if (std::any_of(m_controllers.cbegin(), m_controllers.cend(), [&path](const auto& c) { return c.second && c.second->path == path; }))
			continue;

#ifdef _WIN32
		if (m_type == pad_handler::move)
		{
			check_add_device(nullptr, path, m_enumerated_serials[path]);
			continue;
		}
#endif

#ifdef ANDROID
		if (hid_device* dev = hid_libusb_wrap_sys_device(path, -1))
#elif defined(__APPLE__)
		hid_device* dev = nullptr;
		Emu.BlockingCallFromMainThread([&]()
		{
			dev = hid_open_path(path.c_str());
		}, false);
		if (dev)
#else
		if (hid_device* dev = hid_open_path(path.c_str()))
#endif
		{
			if (const hid_device_info* info = hid_get_device_info(dev))
			{
				hid_log.notice("%s adding device: vid=0x%x, pid=0x%x, path='%s'", m_type, info->vendor_id, info->product_id, path);
			}
			else
			{
				hid_log.warning("%s adding device: vid=N/A, pid=N/A, path='%s', error='%s'", m_type, path, hid_error(dev));
			}

			check_add_device(dev, path, m_enumerated_serials[path]);
		}
		else
		{
			hid_log.error("%s hid_open_path failed! error='%s', path='%s'", m_type, hid_error(dev), path);
			warn_about_drivers = true;
		}
	}

	if (warn_about_drivers)
	{
		hid_log.error("One or more %s pads were detected but couldn't be interacted with directly", m_type);
#if defined(_WIN32) || defined(__linux__)
		hid_log.error("Check https://wiki.rpcs3.net/index.php?title=Help:Controller_Configuration for instructions on how to solve this issue");
#endif
	}
	else
	{
		const usz count = std::count_if(m_controllers.cbegin(), m_controllers.cend(), [](const auto& c) { return c.second && c.second->hidDevice; });
		if (count > 0)
		{
			hid_log.success("%s Controllers found: %d", m_type, count);
		}
		else
		{
			hid_log.warning("No %s controllers found!", m_type);
		}
	}
}

template <class Device>
std::shared_ptr<Device> hid_pad_handler<Device>::get_hid_device(const std::string& padId)
{
	if (!Init())
		return nullptr;

	std::lock_guard devices_lock(m_devices_mutex);

	// Controllers 1-n in GUI
	if (auto it = m_controllers.find(padId); it != m_controllers.end())
	{
		return it->second;
	}

	return nullptr;
}

template <class Device>
std::shared_ptr<PadDevice> hid_pad_handler<Device>::get_device(const std::string& device)
{
	return get_hid_device(device);
}

template <class Device>
u32 hid_pad_handler<Device>::get_battery_color(u8 battery_level, u32 brightness)
{
	static constexpr std::array<u32, 12> battery_level_clr = {0xff00, 0xff33, 0xff66, 0xff99, 0xffcc, 0xffff, 0xccff, 0x99ff, 0x66ff, 0x33ff, 0x00ff, 0x00ff};

	const u32 combined_color = battery_level_clr[battery_level < battery_level_clr.size() ? battery_level : 0];

	const u32 red = (combined_color >> 8) * brightness / 100;
	const u32 green = (combined_color & 0xff) * brightness / 100;
	return ((red << 8) | green);
}

template class hid_pad_handler<ds3_device>;
template class hid_pad_handler<DS4Device>;
template class hid_pad_handler<DualSenseDevice>;
template class hid_pad_handler<skateboard_device>;
template class hid_pad_handler<ps_move_device>;
