//  Copyright 2015 Google Inc. All Rights Reserved.
//
//  Licensed under the Apache License, Version 2.0 (the "License");
//  you may not use this file except in compliance with the License.
//  You may obtain a copy of the License at
//
//  http ://www.apache.org/licenses/LICENSE-2.0
//
//  Unless required by applicable law or agreed to in writing, software
//  distributed under the License is distributed on an "AS IS" BASIS,
//  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
//  See the License for the specific language governing permissions and
//  limitations under the License.

#include "stdafx.h"
#include "ReparsePoint.h"
#include "ScopedHandle.h"
#include "typed_buffer.h"
#include <string>
#include <vector>

// Taken from ntifs.h
#define SYMLINK_FLAG_RELATIVE   1

typedef struct _REPARSE_DATA_BUFFER {
	ULONG  ReparseTag;
	USHORT ReparseDataLength;
	USHORT Reserved;
	union {
		struct {
			USHORT SubstituteNameOffset;
			USHORT SubstituteNameLength;
			USHORT PrintNameOffset;
			USHORT PrintNameLength;
			ULONG Flags;
			WCHAR PathBuffer[1];
		} SymbolicLinkReparseBuffer;
		struct {
			USHORT SubstituteNameOffset;
			USHORT SubstituteNameLength;
			USHORT PrintNameOffset;
			USHORT PrintNameLength;
			WCHAR PathBuffer[1];
		} MountPointReparseBuffer;
		struct {
			UCHAR  DataBuffer[1];
		} GenericReparseBuffer;
	} DUMMYUNIONNAME;
} REPARSE_DATA_BUFFER, * PREPARSE_DATA_BUFFER;

#define REPARSE_DATA_BUFFER_HEADER_LENGTH FIELD_OFFSET(REPARSE_DATA_BUFFER, GenericReparseBuffer.DataBuffer)

#define IO_REPARSE_TAG_MOUNT_POINT              (0xA0000003L)       // winnt
#define IO_REPARSE_TAG_HSM                      (0xC0000004L)       // winnt
#define IO_REPARSE_TAG_DRIVE_EXTENDER           (0x80000005L)
#define IO_REPARSE_TAG_HSM2                     (0x80000006L)       // winnt
#define IO_REPARSE_TAG_SIS                      (0x80000007L)       // winnt
#define IO_REPARSE_TAG_WIM                      (0x80000008L)       // winnt
#define IO_REPARSE_TAG_CSV                      (0x80000009L)       // winnt
#define IO_REPARSE_TAG_DFS                      (0x8000000AL)       // winnt
#define IO_REPARSE_TAG_FILTER_MANAGER           (0x8000000BL)
#define IO_REPARSE_TAG_SYMLINK                  (0xA000000CL)       // winnt
#define IO_REPARSE_TAG_IIS_CACHE                (0xA0000010L)
#define IO_REPARSE_TAG_DFSR                     (0x80000012L)       // winnt
#define IO_REPARSE_TAG_DEDUP                    (0x80000013L)       // winnt
#define IO_REPARSE_TAG_APPXSTRM                 (0xC0000014L)
#define IO_REPARSE_TAG_NFS                      (0x80000014L)       // winnt
#define IO_REPARSE_TAG_FILE_PLACEHOLDER         (0x80000015L)       // winnt
#define IO_REPARSE_TAG_DFM                      (0x80000016L)
#define IO_REPARSE_TAG_WOF                      (0x80000017L)       // winnt

static int g_last_error = 0;

int ReparsePoint::GetLastError()
{
	return g_last_error;
}

ScopedHandle OpenReparsePoint(const std::wstring& path, bool writable)
{
	HANDLE h = CreateFileW(path.c_str(),
		GENERIC_READ | (writable ? GENERIC_WRITE : 0),
		0,
		0,
		OPEN_EXISTING,
		FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
		0);

	if (h == INVALID_HANDLE_VALUE)
	{
		g_last_error = GetLastError();
	}

	return ScopedHandle(h, false);
}

static bool SetReparsePoint(const ScopedHandle& handle, typed_buffer_ptr<REPARSE_DATA_BUFFER>& reparse_buffer)
{
	DWORD cb;
	if (!handle.IsValid()) {
		return false;
	}

	bool ret = DeviceIoControl(handle, FSCTL_SET_REPARSE_POINT,
		reparse_buffer, reparse_buffer.size(), nullptr, 0, &cb, nullptr) == TRUE;
	if (!ret)
	{
		g_last_error = GetLastError();
	}

	return ret;
}

static bool DeleteReparsePoint(const ScopedHandle& handle, PREPARSE_GUID_DATA_BUFFER reparse_buffer)
{
	DWORD cb;
	if (!handle.IsValid()) {
		return false;
	}

	bool ret = DeviceIoControl(handle,
		FSCTL_DELETE_REPARSE_POINT,
		reparse_buffer,
		REPARSE_GUID_DATA_BUFFER_HEADER_SIZE,
		nullptr,
		0,
		&cb,
		0) == TRUE;

	if (!ret)
	{
		g_last_error = GetLastError();
	}

	return ret;
}

typed_buffer_ptr<REPARSE_DATA_BUFFER> BuildMountPoint(const std::wstring& target, const std::wstring& printname)
{
	const size_t target_byte_size = target.size() * 2;
	const size_t printname_byte_size = printname.size() * 2;
	const size_t path_buffer_size = target_byte_size + printname_byte_size + 8 + 4;
	const size_t total_size = path_buffer_size + REPARSE_DATA_BUFFER_HEADER_LENGTH;
	typed_buffer_ptr<REPARSE_DATA_BUFFER> buffer(total_size);

	buffer->ReparseTag = IO_REPARSE_TAG_MOUNT_POINT;
	buffer->ReparseDataLength = static_cast<USHORT>(path_buffer_size);
	buffer->Reserved = 0;

	buffer->MountPointReparseBuffer.SubstituteNameOffset = 0;
	buffer->MountPointReparseBuffer.SubstituteNameLength = static_cast<USHORT>(target_byte_size);
	memcpy(buffer->MountPointReparseBuffer.PathBuffer, target.c_str(), target_byte_size + 2);
	buffer->MountPointReparseBuffer.PrintNameOffset = static_cast<USHORT>(target_byte_size + 2);
	buffer->MountPointReparseBuffer.PrintNameLength = static_cast<USHORT>(printname_byte_size);
	memcpy(buffer->MountPointReparseBuffer.PathBuffer + target.size() + 1, printname.c_str(), printname_byte_size + 2);

	return buffer;
}

typed_buffer_ptr<REPARSE_DATA_BUFFER> BuildSymlink(const std::wstring& target, const std::wstring& printname, bool relative)
{
	const size_t target_byte_size = target.size() * 2;
	const size_t printname_byte_size = printname.size() * 2;
	const size_t path_buffer_size = target_byte_size + printname_byte_size + 12 + 4;
	const size_t total_size = path_buffer_size + REPARSE_DATA_BUFFER_HEADER_LENGTH;
	typed_buffer_ptr<REPARSE_DATA_BUFFER> buffer(total_size);

	buffer->ReparseTag = IO_REPARSE_TAG_SYMLINK;
	buffer->ReparseDataLength = static_cast<USHORT>(path_buffer_size);
	buffer->Reserved = 0;

	buffer->SymbolicLinkReparseBuffer.SubstituteNameOffset = 0;
	buffer->SymbolicLinkReparseBuffer.SubstituteNameLength = static_cast<USHORT>(target_byte_size);
	memcpy(buffer->SymbolicLinkReparseBuffer.PathBuffer, target.c_str(), target_byte_size + 2);
	buffer->SymbolicLinkReparseBuffer.PrintNameOffset = static_cast<USHORT>(target_byte_size + 2);
	buffer->SymbolicLinkReparseBuffer.PrintNameLength = static_cast<USHORT>(printname_byte_size);
	memcpy(buffer->SymbolicLinkReparseBuffer.PathBuffer + target.size() + 1, printname.c_str(), printname_byte_size + 2);
	buffer->SymbolicLinkReparseBuffer.Flags = relative ? SYMLINK_FLAG_RELATIVE : 0;

	return buffer;
}

static bool CreateMountPointInternal(const std::wstring& path, typed_buffer_ptr<REPARSE_DATA_BUFFER>& buffer)
{
	ScopedHandle handle = OpenReparsePoint(path, true);

	if (!handle.IsValid())
	{
		return false;
	}

	return SetReparsePoint(handle, buffer);
}

static bool CreateMountPointInternal(const ScopedHandle& handle, typed_buffer_ptr<REPARSE_DATA_BUFFER>& buffer)
{
	return SetReparsePoint(handle, buffer);
}

std::wstring FixupPath(std::wstring str)
{
	if (str[0] != '\\')
	{
		return L"\\??\\" + str;
	}

	return str;
}

bool ReparsePoint::CreateMountPoint(const std::wstring& path, const std::wstring& target, const std::wstring& printname)
{
	if (target.length() == 0)
	{
		return false;
	}

	return CreateMountPointInternal(path, BuildMountPoint(FixupPath(target), printname));
}

bool ReparsePoint::CreateSymlink(const std::wstring& path, const std::wstring& target, const std::wstring& printname, bool relative)
{
	if (target.length() == 0)
	{
		return false;
	}

	return CreateMountPointInternal(path, BuildSymlink(!relative ? FixupPath(target) : target, printname, relative));
}

bool ReparsePoint::CreateSymlink(HANDLE h, const std::wstring& target, const std::wstring& printname, bool relative)
{
	ScopedHandle handle(h, true);

	if (!handle.IsValid())
	{
		return false;
	}

	return CreateMountPointInternal(handle, BuildSymlink(!relative ? FixupPath(target) : target, printname, relative));
}

bool ReparsePoint::DeleteMountPoint(const std::wstring& path)
{
	REPARSE_GUID_DATA_BUFFER reparse_buffer = { 0 };
	reparse_buffer.ReparseTag = IO_REPARSE_TAG_MOUNT_POINT;

	ScopedHandle handle = OpenReparsePoint(path, true);

	return DeleteReparsePoint(handle, &reparse_buffer);
}

bool ReparsePoint::CreateRawMountPoint(const std::wstring& path, DWORD reparse_tag, const std::vector<BYTE>& buffer)
{
	typed_buffer_ptr<REPARSE_DATA_BUFFER> reparse_buffer(8 + buffer.size());

	reparse_buffer->ReparseTag = reparse_tag;
	reparse_buffer->ReparseDataLength = static_cast<USHORT>(buffer.size());
	reparse_buffer->Reserved = 0;
	memcpy(reparse_buffer->GenericReparseBuffer.DataBuffer, &buffer[0], buffer.size());

	return CreateMountPointInternal(path, reparse_buffer);
}

static typed_buffer_ptr<REPARSE_DATA_BUFFER> GetReparsePointData(ScopedHandle handle)
{
	typed_buffer_ptr<REPARSE_DATA_BUFFER> buf(MAXIMUM_REPARSE_DATA_BUFFER_SIZE);

	DWORD dwBytesReturned;
	if (!DeviceIoControl(handle,
		FSCTL_GET_REPARSE_POINT,
		NULL,
		0,
		(LPVOID)buf,
		buf.size(),
		&dwBytesReturned,
		0)
		)
	{
		g_last_error = GetLastError();
		buf.reset(0);
	}

	return buf;
}

std::wstring ReparsePoint::GetMountPointTarget(const std::wstring& path)
{
	ScopedHandle handle = OpenReparsePoint(path, false);
	if (!handle.IsValid())
	{
		return L"";
	}

	typed_buffer_ptr<REPARSE_DATA_BUFFER> buf = GetReparsePointData(handle);

	if (buf.size() == 0)
	{
		return L"";
	}

	if (buf->ReparseTag != IO_REPARSE_TAG_MOUNT_POINT)
	{
		g_last_error = ERROR_REPARSE_TAG_MISMATCH;
		return L"";
	}

	WCHAR* base = &buf->MountPointReparseBuffer.PathBuffer[buf->MountPointReparseBuffer.SubstituteNameOffset / 2];

	return std::wstring(base, base + (buf->MountPointReparseBuffer.SubstituteNameLength / 2));
}

bool ReparsePoint::IsReparsePoint(const std::wstring& path)
{
	ScopedHandle handle = OpenReparsePoint(path, false);
	BY_HANDLE_FILE_INFORMATION file_info = { 0 };

	return handle.IsValid() && GetFileInformationByHandle(handle, &file_info) && file_info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT;
}

static bool ReadReparsePoint(const std::wstring& path, typed_buffer_ptr<REPARSE_DATA_BUFFER>& reparse_buffer)
{
	ScopedHandle handle = OpenReparsePoint(path, false);
	reparse_buffer.reset(4096);
	DWORD dwSize;

	bool ret = DeviceIoControl(handle, FSCTL_GET_REPARSE_POINT, nullptr, 0, reparse_buffer, reparse_buffer.size(), &dwSize, nullptr) == TRUE;
	if (!ret)
	{
		g_last_error = GetLastError();
		return false;
	}
	else
	{
		reparse_buffer.resize(dwSize);
		return true;
	}
}

static bool IsReparseTag(const std::wstring& path, DWORD reparse_tag)
{
	typed_buffer_ptr<REPARSE_DATA_BUFFER> buffer;

	if (ReadReparsePoint(path, buffer))
	{
		return buffer->ReparseTag == reparse_tag;
	}
	else
	{
		return false;
	}
}

bool ReparsePoint::IsMountPoint(const std::wstring& path)
{
	return IsReparseTag(path, IO_REPARSE_TAG_MOUNT_POINT);
}

bool ReparsePoint::IsSymlink(const std::wstring& path)
{
	return IsReparseTag(path, IO_REPARSE_TAG_SYMLINK);
}

bool ReparsePoint::ReadMountPoint(const std::wstring& path, std::wstring& target, std::wstring& printname)
{
	typed_buffer_ptr<REPARSE_DATA_BUFFER> buffer;

	if (ReadReparsePoint(path, buffer) && buffer->ReparseTag == IO_REPARSE_TAG_MOUNT_POINT)
	{
		WCHAR* target_name = &buffer->MountPointReparseBuffer.PathBuffer[buffer->MountPointReparseBuffer.SubstituteNameOffset / 2];
		WCHAR* display_name = &buffer->MountPointReparseBuffer.PathBuffer[buffer->MountPointReparseBuffer.PrintNameOffset / 2];
		target.assign(target_name, target_name + buffer->MountPointReparseBuffer.SubstituteNameLength / 2);
		printname.assign(display_name, display_name + buffer->MountPointReparseBuffer.PrintNameLength / 2);
		return true;
	}
	else
	{
		return false;
	}
}

bool ReparsePoint::ReadSymlink(const std::wstring& path, std::wstring& target, std::wstring& printname, unsigned int* flags)
{
	typed_buffer_ptr<REPARSE_DATA_BUFFER> buffer;

	if (ReadReparsePoint(path, buffer) && buffer->ReparseTag == IO_REPARSE_TAG_SYMLINK)
	{
		WCHAR* target_name = &buffer->SymbolicLinkReparseBuffer.PathBuffer[buffer->SymbolicLinkReparseBuffer.SubstituteNameOffset / 2];
		WCHAR* display_name = &buffer->SymbolicLinkReparseBuffer.PathBuffer[buffer->SymbolicLinkReparseBuffer.PrintNameOffset / 2];
		target.assign(target_name, target_name + buffer->SymbolicLinkReparseBuffer.SubstituteNameLength / 2);
		printname.assign(display_name, display_name + buffer->SymbolicLinkReparseBuffer.PrintNameLength / 2);
		*flags = buffer->SymbolicLinkReparseBuffer.Flags;
		return true;
	}
	else
	{
		return false;
	}
}

bool ReparsePoint::ReadRaw(const std::wstring& path, unsigned int* reparse_tag, std::vector<BYTE>& raw_data)
{
	typed_buffer_ptr<REPARSE_DATA_BUFFER> buffer;

	if (ReadReparsePoint(path, buffer))
	{
		*reparse_tag = buffer->ReparseTag;
		raw_data.resize(buffer->ReparseDataLength);
		memcpy(&raw_data[0], buffer->GenericReparseBuffer.DataBuffer, buffer->ReparseDataLength);
		return true;
	}
	else
	{
		return false;
	}

	return false;
}
