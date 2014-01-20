###| CMake Kiibohd Controller Debug Module |###
#
# Written by Jacob Alexander in 2014 for the Kiibohd Controller
#
# Released into the Public Domain
#
###


###
# Module C files
#

set( DEBUG_SRCS
	cli.c
)


###
# Setup File Dependencies
#


###
# Module Specific Options
#


###
# Just in case, you only want this module and are using others as well
#
add_definitions( -I${HEAD_DIR}/Debug/off )


###
# Compiler Family Compatibility
#
set( DebugModuleCompatibility
	arm
	avr
)

