#include "kdmapper.hpp"
#include "buffer.h"

uint64_t kdmapper::MapDriver(HANDLE iqvw64e_device_handle, ULONG64 param1, ULONG64 param2, bool free, bool destroyHeader, bool PassAllocationAddressAsFirstParam) {
	
	char* driverbuffer = (char*)&driverx00_sys[0];
	int lenght = driverx00_sys_len;
	std::vector<uint8_t> raw_image(driverbuffer, driverbuffer + lenght);

	const PIMAGE_NT_HEADERS64 nt_headers = portable_executable::GetNtHeaders(raw_image.data());

	if (!nt_headers) {
		Log("[-] Invalid format of PE image") << std::endl;
		return 0;
	}

	if (nt_headers->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
		Log("[-] Image is not 64 bit") << std::endl;
		return 0;
	}

	const uint32_t image_size = nt_headers->OptionalHeader.SizeOfImage;

	void* local_image_base = VirtualAlloc(nullptr, image_size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
	if (!local_image_base)
		return 0;

	DWORD TotalVirtualHeaderSize = (IMAGE_FIRST_SECTION(nt_headers))->VirtualAddress;

	uint64_t kernel_image_base = 0;
	kernel_image_base = intel_driver::AllocatePool(iqvw64e_device_handle, nt::POOL_TYPE::NonPagedPool, image_size - (destroyHeader ? TotalVirtualHeaderSize : 0));

	do {
		if (!kernel_image_base) {
			Log("[-] Failed to allocate remote image in kernel") << std::endl;
			break;
		}

		Log("[+] Image base has been allocated at 0x") << reinterpret_cast<void*>(kernel_image_base) << std::endl;

		// Copy image headers

		memcpy(local_image_base, raw_image.data(), nt_headers->OptionalHeader.SizeOfHeaders);

		// Copy image sections

		const PIMAGE_SECTION_HEADER current_image_section = IMAGE_FIRST_SECTION(nt_headers);

		for (auto i = 0; i < nt_headers->FileHeader.NumberOfSections; ++i) {
			auto local_section = reinterpret_cast<void*>(reinterpret_cast<uint64_t>(local_image_base) + current_image_section[i].VirtualAddress);
			memcpy(local_section, reinterpret_cast<void*>(reinterpret_cast<uint64_t>(raw_image.data()) + current_image_section[i].PointerToRawData), current_image_section[i].SizeOfRawData);
		}

		uint64_t realBase = kernel_image_base;
		if (destroyHeader) {
			kernel_image_base -= TotalVirtualHeaderSize;
			Log("[+] Skipped 0x") << std::hex << TotalVirtualHeaderSize << " bytes of PE Header" << std::endl;
		}

		// Resolve relocs and imports

		RelocateImageByDelta(portable_executable::GetRelocs(local_image_base), kernel_image_base - nt_headers->OptionalHeader.ImageBase);

		if (!ResolveImports(iqvw64e_device_handle, portable_executable::GetImports(local_image_base))) {
			Log("[-] Failed to resolve imports") << std::endl;
			kernel_image_base = realBase;
			break;
		}

		// Write fixed image to kernel

		if (!intel_driver::WriteMemory(iqvw64e_device_handle, realBase, (PVOID)((uintptr_t)local_image_base + (destroyHeader ? TotalVirtualHeaderSize : 0)), image_size - (destroyHeader ? TotalVirtualHeaderSize : 0))) {
			Log("[-] Failed to write local image to remote image") << std::endl;
			kernel_image_base = realBase;
			break;
		}

		// Call driver entry point

		const uint64_t address_of_entry_point = kernel_image_base + nt_headers->OptionalHeader.AddressOfEntryPoint;

		Log("[<] Calling DriverEntry 0x") << reinterpret_cast<void*>(address_of_entry_point) << std::endl;

		NTSTATUS status = 0;

		if (!intel_driver::CallKernelFunction(iqvw64e_device_handle, &status, address_of_entry_point, (PassAllocationAddressAsFirstParam ? realBase : param1), param2)) {
			Log("[-] Failed to call driver entry") << std::endl;
			kernel_image_base = realBase;
			break;
		}

		Log("[+] DriverEntry returned 0x") << std::hex << status << std::endl;

		if (free)
			intel_driver::FreePool(iqvw64e_device_handle, realBase);


		VirtualFree(local_image_base, 0, MEM_RELEASE);
		return realBase;

	} while (false);


	VirtualFree(local_image_base, 0, MEM_RELEASE);

	intel_driver::FreePool(iqvw64e_device_handle, kernel_image_base);
	
	return 0;
}

void kdmapper::RelocateImageByDelta(portable_executable::vec_relocs relocs, const uint64_t delta) {
	
	for (const auto& current_reloc : relocs) {
		for (auto i = 0u; i < current_reloc.count; ++i) {
			const uint16_t type = current_reloc.item[i] >> 12;
			const uint16_t offset = current_reloc.item[i] & 0xFFF;

			if (type == IMAGE_REL_BASED_DIR64)
				*reinterpret_cast<uint64_t*>(current_reloc.address + offset) += delta;
		}
	}
	
}

bool kdmapper::ResolveImports(HANDLE iqvw64e_device_handle, portable_executable::vec_imports imports) {
	
	for (const auto& current_import : imports) {
		ULONG64 Module = utils::GetKernelModuleAddress(current_import.module_name);
		if (!Module) {
#if !defined(DISABLE_OUTPUT)
			std::cout << "[-] Dependency " << current_import.module_name << " wasn't found" << std::endl;
#endif
			return false;
		}

		for (auto& current_function_data : current_import.function_datas) {
			uint64_t function_address = intel_driver::GetKernelModuleExport(iqvw64e_device_handle, Module, current_function_data.name);

			if (!function_address) {
				//Lets try with ntoskrnl
				if (Module != intel_driver::ntoskrnlAddr) {
					function_address = intel_driver::GetKernelModuleExport(iqvw64e_device_handle, intel_driver::ntoskrnlAddr, current_function_data.name);
					if (!function_address) {
#if !defined(DISABLE_OUTPUT)
						std::cout << "[-] Failed to resolve import " << current_function_data.name << " (" << current_import.module_name << ")" << std::endl;
#endif
						return false;
					}
				}
			}

			*current_function_data.address = function_address;
		}
	}
	
	return true;
}
