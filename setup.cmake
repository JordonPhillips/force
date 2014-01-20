###| CMAKE Kiibohd Controller Source Configurator |###
#
# Written by Jacob Alexander in 2011-2014 for the Kiibohd Controller
#
# Released into the Public Domain
#
###



###
# Project Modules
#

##| Sends the current list of usb key codes through USB HID
set( OutputModule  "pjrcUSB" )

##| Debugging source to use, each module has it's own set of defines that it sets
set( DebugModule   "full"    )




###
# Module Overrides (Used in the buildall.bash script)
#
if ( ( DEFINED ScanModuleOverride ) AND ( EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/Scan/${ScanModuleOverride} ) )
	set( ScanModule ${ScanModuleOverride} )
endif ()



###
# Path Setup
#
set( OutputModulePath "Output/${OutputModule}" )
set(  DebugModulePath  "Debug/${DebugModule}"  )

#| Top-level directory adjustment
set( HEAD_DIR "${CMAKE_CURRENT_SOURCE_DIR}" )



###
# Module Check Function
#

#| Usage:
#|  PathPrepend( ModulePath <ListOfFamiliesSupported> )
#| Uses the ${COMPILER_FAMILY} variable
function( ModuleCompatibility ModulePath )
	foreach( mod_var ${ARGN} )
		if ( ${mod_var} STREQUAL ${COMPILER_FAMILY} )
			# Module found, no need to scan further
			return()
		endif ()
	endforeach()

	message( FATAL_ERROR "${ModulePath} does not support the ${COMPILER_FAMILY} family..." )
endfunction()



###
# Module Configuration
#

#| Additional options, usually define settings
add_definitions()

#| Include path for each of the modules
add_definitions(
	-I${HEAD_DIR}/${OutputModulePath}
	-I${HEAD_DIR}/${DebugModulePath}
)




###
# Module Processing
#

#| Go through lists of sources and append paths
#| Usage:
#|  PathPrepend( OutputListOfSources <Prepend Path> <InputListOfSources> )
macro( PathPrepend Output SourcesPath )
	unset( tmpSource )

	# Loop through items
	foreach( item ${ARGN} )
		# Set the path
		set( tmpSource ${tmpSource} "${SourcesPath}/${item}" )
	endforeach()

	# Finalize by writing the new list back over the old one
	set( ${Output} ${tmpSource} )
endmacro()


#| Output Module
include    (             "${OutputModulePath}/setup.cmake"   )
PathPrepend( OUTPUT_SRCS  ${OutputModulePath} ${OUTPUT_SRCS} )

#| Debugging Module
include    (           "${DebugModulePath}/setup.cmake"  )
PathPrepend( DEBUG_SRCS ${DebugModulePath} ${DEBUG_SRCS} )


#| Print list of all module sources
message( STATUS "Detected USB Module Source Files:" )
message( "${OUTPUT_SRCS}" )
message( STATUS "Detected Debug Module Source Files:" )
message( "${DEBUG_SRCS}" )



###
# Generate USB Defines
#

#| Manufacturer name
set( MANUFACTURER "Kiibohd" )


#| Serial Number
#| Attempt to call Git to get the branch, last commit date, and whether code modified since last commit

#| Modified
#| Takes a bit of work to extract the "M " using CMake, and not using it if there are no modifications
execute_process( COMMAND git status -s -uno --porcelain
	WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
	OUTPUT_VARIABLE Git_Modified_INFO
	ERROR_QUIET
	OUTPUT_STRIP_TRAILING_WHITESPACE
)
string( LENGTH "${Git_Modified_INFO}" Git_Modified_LENGTH )
if ( ${Git_Modified_LENGTH} GREATER 2 )
	string( SUBSTRING "${Git_Modified_INFO}" 1 2 Git_Modified_Flag_INFO )
endif ()

#| Branch
execute_process( COMMAND git rev-parse --abbrev-ref HEAD
	WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
	OUTPUT_VARIABLE Git_Branch_INFO
	ERROR_QUIET
	OUTPUT_STRIP_TRAILING_WHITESPACE
)

#| Date
execute_process( COMMAND git show -s --format=%ci
	WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
	OUTPUT_VARIABLE Git_Date_INFO
	RESULT_VARIABLE Git_RETURN
	ERROR_QUIET
	OUTPUT_STRIP_TRAILING_WHITESPACE
)


#| Only use Git variables if we were successful in calling the commands
if ( ${Git_RETURN} EQUAL 0 )
	set( GitLastCommitDate "${Git_Modified_Flag_INFO}${Git_Branch_INFO} - ${Git_Date_INFO}" )
else ()
	# TODO Figure out a good way of finding the current branch + commit date + modified
	set( GitLastCommitDate "Pft...Windows Build" )
endif ()


#| Uses CMake variables to include as defines
#| Primarily for USB configuration
configure_file( ${CMAKE_CURRENT_SOURCE_DIR}/Lib/_buildvars.h buildvars.h )

