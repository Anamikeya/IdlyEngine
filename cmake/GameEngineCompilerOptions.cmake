# Warning and diagnostic settings for engine-owned targets.
# Third-party targets keep their own flags; never apply these globally.

function(gameengine_target_defaults target)
	target_compile_features(${target} PUBLIC cxx_std_20)

	if(MSVC)
		target_compile_options(${target} PRIVATE
			/W4
			/permissive-
			/Zc:__cplusplus
			/Zc:preprocessor
			/utf-8
			/w14242 # conversion, possible loss of data
			/w14254 # larger bit field to smaller bit field
			/w14263 # member function hides base virtual
			/w14265 # class has virtual functions but non-virtual destructor
			/w14287 # unsigned/negative constant mismatch
			/w14296 # expression is always true/false
			/w14311 # pointer truncation
			/w14826 # sign-extending conversion
			/w14905 /w14906 # string literal casts
			/w14928 # illegal copy-initialization
		)
		target_compile_definitions(${target} PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)
		if(GAMEENGINE_WARNINGS_AS_ERRORS)
			target_compile_options(${target} PRIVATE /WX)
		endif()
	else()
		target_compile_options(${target} PRIVATE
			-Wall
			-Wextra
			-Wpedantic
			-Wshadow
			-Wconversion
			-Wsign-conversion
			-Wnon-virtual-dtor
			-Wold-style-cast
			-Wcast-align
			-Woverloaded-virtual
			-Wnull-dereference
			-Wdouble-promotion
		)
		if(GAMEENGINE_WARNINGS_AS_ERRORS)
			target_compile_options(${target} PRIVATE -Werror)
		endif()
	endif()
endfunction()
