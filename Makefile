CXXFLAGS:=-std=c++14  -Wstrict-aliasing=2 -Wsequence-point -Warray-bounds -Wextra -Winit-self -Wformat=2 -Wno-format-nonliteral -Wformat-security \
		-Wunused-variable -Wunused-value -Wreturn-type -Wparentheses -Wmissing-braces -Wno-invalid-source-encoding -Wno-invalid-offsetof \
		-Wno-unknown-pragmas -Wno-missing-field-initializers -Wno-unused-parameter -Wno-sign-compare -Wno-invalid-offsetof   \
		-fno-rtti -std=c++14 -ffast-math  -D_REENTRANT -DREENTRANT  -g3 -ggdb -fno-omit-frame-pointer   \
		-fno-strict-aliasing    -DLEAN_SWITCH  -ISwitch/ -Wno-maybe-uninitialized -Wno-unused-function -Wno-uninitialized -funroll-loops  -O3
all: client

client: coroutines.o
	ar rcs libcoroutines.a  coroutines.o

.o: .cpp

clean:
	rm -f *.o *.a

.PHONY: clean
