# Warning sets shared by all targets.
#
# rufus_warnings:  strict set for Rufus' own code (optionally fatal).
# vendor_warnings: relaxed set for vendored third-party code, which we try to
#                  keep as close to upstream as possible and never make fatal.

add_library(rufus_warnings INTERFACE)
target_compile_options(rufus_warnings INTERFACE
	-Wall -Wextra -Wshadow -Wformat-security -Wundef -Wunused
	-Wno-unused-parameter -Wno-sign-compare
	$<$<COMPILE_LANGUAGE:C>:-Wstrict-prototypes -Werror=implicit-function-declaration>)
if(RUFUS_WERROR)
	target_compile_options(rufus_warnings INTERFACE -Werror)
endif()

add_library(vendor_warnings INTERFACE)
target_compile_options(vendor_warnings INTERFACE
	-Wall -Wno-unused-parameter -Wno-sign-compare -Wno-pointer-sign
	$<$<COMPILE_LANGUAGE:C>:-Werror=implicit-function-declaration>)
