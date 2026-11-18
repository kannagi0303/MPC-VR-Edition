#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iterator>

namespace jvr {

struct Eye {
	float x = 0.5f;
	float y = 0.5f;
	float focal = 299.3f / 1024;
};

struct Profile {
	Eye eyes[2];
};

inline constexpr int AspectWidth = 16;
inline constexpr int AspectHeight = 9;
// 110 degrees vertically with a unit-scale stereographic projection.
inline constexpr double PlaneHalfHeight = 1.0411341011034925; // 2 * tan(110 degrees / 4)
inline constexpr double PlaneHalfWidth = PlaneHalfHeight * AspectWidth / AspectHeight;

inline int FrameWidth(int height)
{
	return int((std::int64_t(height) * AspectWidth + AspectHeight / 2) / AspectHeight);
}

struct FrameSize {
	int width;
	int height;
};

inline FrameSize FitFrame(int width, int height)
{
	if (width <= 0 || height <= 0) return {};
	if (std::int64_t(width) * AspectHeight >= std::int64_t(height) * AspectWidth) {
		return {FrameWidth(height), height};
	}
	const auto scaledHeight = int((std::int64_t(width) * AspectHeight + AspectWidth / 2) / AspectWidth);
	return {width, std::max(1, scaledHeight)};
}

// Match the centre sampling density of the built-in profile. Keep this
// canvas fixed while the user moves the virtual camera or changes profile.
inline FrameSize SourceFrameSize(int eyeHeight)
{
	if (eyeHeight <= 0) return {};
	constexpr double density = 2 * PlaneHalfHeight * Eye{}.focal;
	const auto units = std::max(1, int(std::lround(eyeHeight * density / AspectHeight)));
	return {units * AspectWidth, units * AspectHeight};
}

struct ViewTransform {
	float cosPitch = 1.0f;
	float sinPitch = 0.0f;
	float verticalShift = 0.0f;
};

inline ViewTransform MakeView(int verticalPercent, int reprojectionPercent)
{
	// Choose a centre ray from the old crop offset. Split its angle between camera
	// pitch and a residual shift in the pitched stereographic plane.
	const double angle = 2 * std::atan(PlaneHalfHeight * verticalPercent / 100.0);
	const double pitch = angle * (reprojectionPercent / 100.0);
	return {float(std::cos(pitch)), float(std::sin(pitch)), float(2 * std::tan((angle - pitch) / 2))};
}

} // namespace jvr

#ifdef _WIN32
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace jvr {

enum class ProjectionType { JvrSbs };

struct MediaProfile {
	std::wstring name;
	ProjectionType type;
};

inline bool EqualsIgnoreCase(std::wstring_view first, std::wstring_view second)
{
	return first.size() == second.size() &&
		CompareStringOrdinal(first.data(), static_cast<int>(first.size()),
			second.data(), static_cast<int>(second.size()), TRUE) == CSTR_EQUAL;
}

inline std::vector<std::wstring> ProfileFiles(HMODULE module)
{
	std::vector<std::wstring> files;
	std::wstring directory(32768, L'\0');
	const DWORD length = GetModuleFileNameW(module, directory.data(), static_cast<DWORD>(directory.size()));
	if (!length || length >= directory.size()) return files;
	directory.resize(length);
	const auto separator = directory.find_last_of(L"\\/");
	if (separator == std::wstring::npos) return files;
	directory.resize(separator + 1);
	directory += L"VRProfiles\\";

	WIN32_FIND_DATAW data{};
	const HANDLE search = FindFirstFileW((directory + L"*.ini").c_str(), &data);
	if (search == INVALID_HANDLE_VALUE) return files;
	struct FindHandle {
		HANDLE search;
		~FindHandle() { FindClose(search); }
	} handle{search};
	do {
		const std::wstring_view name(data.cFileName);
		if (!(data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && name.size() >= 4 &&
			EqualsIgnoreCase(name.substr(name.size() - 4), L".ini")) {
			files.emplace_back(directory + data.cFileName);
		}
	} while (FindNextFileW(search, &data));
	std::sort(files.begin(), files.end());
	return files;
}

inline std::wstring ReadProfileField(const std::wstring& path, LPCWSTR section, LPCWSTR key)
{
	wchar_t value[512]{};
	const DWORD length = GetPrivateProfileStringW(section, key, L"", value,
		static_cast<DWORD>(std::size(value)), path.c_str());
	if (!length || length >= std::size(value) - 1) return {};
	return {value, length};
}

inline std::optional<MediaProfile> FindMediaProfile(HMODULE module, std::wstring_view fileName)
{
	if (fileName.empty()) return {};
	std::optional<MediaProfile> match;
	size_t bestPrefix = 0;

	for (const auto& path : ProfileFiles(module)) {
		const auto type = ReadProfileField(path, L"Projection", L"Type");
		if (!EqualsIgnoreCase(type, L"JVR_SBS")) continue;
		auto name = ReadProfileField(path, L"Profile", L"Name");
		if (name.empty()) continue;

		const auto exact = ReadProfileField(path, L"Match", L"FileName");
		if (!exact.empty() && EqualsIgnoreCase(fileName, exact)) {
			return MediaProfile{std::move(name), ProjectionType::JvrSbs};
		}
		const auto prefix = ReadProfileField(path, L"Match", L"StartsWith");
		if (prefix.size() > bestPrefix && fileName.size() >= prefix.size() &&
			EqualsIgnoreCase(fileName.substr(0, prefix.size()), prefix)) {
			bestPrefix = prefix.size();
			match = MediaProfile{std::move(name), ProjectionType::JvrSbs};
		}
	}
	return match;
}

} // namespace jvr
#endif
