override MEISO_OVERLAY_SELF := $(realpath $(lastword $(MAKEFILE_LIST)))
meiso_shell_quote = '$(subst ','"'"',$(1))'

ifeq ($(strip $(value MEISO_PUBLIC_ROOT)),)
$(error MEISO_PUBLIC_ROOT must name the clean, committed meiso_glass checkout)
endif
override MEISO_PUBLIC_ROOT_RESOLVED := $(realpath $(value MEISO_PUBLIC_ROOT))
ifeq ($(MEISO_PUBLIC_ROOT_RESOLVED),)
$(error MEISO_PUBLIC_ROOT does not resolve to an existing directory)
endif
override MEISO_PUBLIC_ROOT_VALID := $(strip $(shell printf '%s\n' $(call meiso_shell_quote,$(MEISO_PUBLIC_ROOT_RESOLVED)) | LC_ALL=C grep -Eq '^/[A-Za-z0-9_./-]+$$'; echo $$?))
ifneq ($(MEISO_PUBLIC_ROOT_VALID),0)
$(error MEISO_PUBLIC_ROOT must be an absolute path using only A-Z, a-z, 0-9, _, ., /, or -)
endif

ifeq ($(strip $(value MEISO_PUBLIC_COMMIT)),)
$(error MEISO_PUBLIC_COMMIT must be the full, locked public source commit)
endif
override MEISO_PUBLIC_ACTUAL_COMMIT := $(strip $(shell git -C $(call meiso_shell_quote,$(MEISO_PUBLIC_ROOT_RESOLVED)) rev-parse --verify HEAD 2>/dev/null))
ifeq ($(MEISO_PUBLIC_ACTUAL_COMMIT),)
$(error MEISO_PUBLIC_ROOT is not a readable Git checkout)
endif
override MEISO_PUBLIC_ACTUAL_COMMIT_VALID := $(strip $(shell printf '%s\n' $(call meiso_shell_quote,$(MEISO_PUBLIC_ACTUAL_COMMIT)) | LC_ALL=C grep -Eq '^[0-9a-f]{40}$$'; echo $$?))
ifneq ($(MEISO_PUBLIC_ACTUAL_COMMIT_VALID),0)
$(error public source HEAD is not a full 40-character lowercase commit)
endif
ifneq ($(value MEISO_PUBLIC_COMMIT),$(MEISO_PUBLIC_ACTUAL_COMMIT))
$(error MEISO_PUBLIC_COMMIT must exactly match the validated public HEAD)
endif
override MEISO_PUBLIC_STATUS := $(strip $(shell git -C $(call meiso_shell_quote,$(MEISO_PUBLIC_ROOT_RESOLVED)) status --porcelain --untracked-files=normal 2>/dev/null))
ifneq ($(MEISO_PUBLIC_STATUS),)
$(error MEISO_PUBLIC_ROOT must be clean before a target candidate build)
endif

override MEISO_EXPECTED_OVERLAY_MAKEFILE := $(MEISO_PUBLIC_ROOT_RESOLVED)/targets/k230d/sdk-overlay/src/applications/meiso_conformance/Makefile
override MEISO_OVERLAY_MATCH := $(strip $(shell cmp -s $(call meiso_shell_quote,$(MEISO_OVERLAY_SELF)) $(call meiso_shell_quote,$(MEISO_EXPECTED_OVERLAY_MAKEFILE)); echo $$?))
ifneq ($(MEISO_OVERLAY_MATCH),0)
$(error staged SDK application Makefile differs from the locked public source)
endif

override MEISO_REQUIRED_FILES := \
	include/meiso/core/resource.h \
	include/meiso/core/state.h \
	include/meiso/core/stream.h \
	include/meiso/core/action.h \
	include/meiso/core/blob.h \
	include/meiso/core/descriptor_set.h \
	src/core/meiso_resource.c \
	src/core/meiso_state.c \
	src/core/meiso_stream.c \
	src/core/meiso_blob.c \
	src/core/meiso_descriptor_set.c \
	src/core/meiso_action.c \
	src/core/meiso_action_private.c \
	src/core/meiso_action_caller.c \
	src/core/meiso_action_executor.c \
	src/core/meiso_action_private.h \
	src/core/meiso_internal.h \
	tests/meiso_test_support.h \
	tests/test_resource.c \
	tests/test_state.c \
	tests/test_stream.c \
	tests/test_action.c \
	tests/test_blob.c \
	tests/test_descriptor_set.c \
	tests/test_four_kind.c \
	targets/conformance/main.c
override MEISO_MISSING_FILES := $(strip $(foreach file,$(MEISO_REQUIRED_FILES),$(if $(wildcard $(MEISO_PUBLIC_ROOT_RESOLVED)/$(file)),,$(file))))
ifneq ($(MEISO_MISSING_FILES),)
$(error locked public source is missing: $(MEISO_MISSING_FILES))
endif

include ../mkenv.mk
include $(SDK_RTSMART_SRC_DIR)/libs/mk/toolchain_riscv64_musl.mk

override DIR := meiso_conformance
override BUILD := $(SDK_APPS_BUILD_DIR)/$(DIR)/$(MEISO_PUBLIC_ACTUAL_COMMIT)
override BIN := $(SDK_APPS_IMAGE_DIR)/$(DIR)
override BIN_DIR := $(dir $(BIN))

override MEISO_CORE_NAMES := \
	meiso_resource \
	meiso_state \
	meiso_stream \
	meiso_blob \
	meiso_descriptor_set \
	meiso_action \
	meiso_action_private \
	meiso_action_caller \
	meiso_action_executor
override MEISO_SUITE_NAMES := \
	resource \
	state \
	stream \
	action \
	blob \
	descriptor_set \
	four_kind
override MEISO_CORE_OBJS := $(addprefix $(BUILD)/core/,$(addsuffix .o,$(MEISO_CORE_NAMES)))
override MEISO_SUITE_OBJS := $(addprefix $(BUILD)/tests/test_,$(addsuffix .o,$(MEISO_SUITE_NAMES)))
override MEISO_RUNNER_OBJ := $(BUILD)/runner/main.o
override OBJS := $(MEISO_CORE_OBJS) $(MEISO_SUITE_OBJS) $(MEISO_RUNNER_OBJ)
override DEPS := $(OBJS:.o=.d)

override CPPFLAGS := \
	-I$(MEISO_PUBLIC_ROOT_RESOLVED)/include \
	-I$(MEISO_PUBLIC_ROOT_RESOLVED)/src/core \
	-I$(MEISO_PUBLIC_ROOT_RESOLVED)/tests \
	-I$(SDK_SRC_ROOT_DIR)/include \
	-include generated/autoconf.h \
	-DMEISO_CONFORMANCE_SOURCE_COMMIT=\"$(MEISO_PUBLIC_ACTUAL_COMMIT)\"
override CFLAGS := \
	-std=c11 \
	-march=rv64imafdcv \
	-mabi=lp64d \
	-mcmodel=medany \
	-Os \
	-Wall \
	-Wextra \
	-Wpedantic \
	-Werror \
	-ffile-prefix-map=$(MEISO_PUBLIC_ROOT_RESOLVED)=meiso_glass \
	-fmacro-prefix-map=$(MEISO_PUBLIC_ROOT_RESOLVED)=meiso_glass \
	-fstack-usage \
	-ffunction-sections \
	-fdata-sections
override LDFLAGS := \
	-T $(SDK_RTSMART_SRC_DIR)/libs/mk/link.lds \
	-static \
	-Wl,--gc-sections \
	-Wl,-Map,$(BUILD)/$(DIR).map

.PHONY: all clean distclean

all: $(BIN)
	@$(ECHO) "[BUILD] applications $(DIR) from $(MEISO_PUBLIC_ACTUAL_COMMIT) done."

$(BIN): $(OBJS) | $(BIN_DIR)
	@$(ECHO) "[LD] $@"
	$(Q)$(CC) $(CFLAGS) $(OBJS) $(LDFLAGS) -o $@
	$(Q)$(SIZE) $@

$(BUILD)/core/%.o: $(MEISO_PUBLIC_ROOT_RESOLVED)/src/core/%.c | $(BUILD)/core
	@$(ECHO) "[CC] $@"
	$(Q)$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -MF $(@:.o=.d) -c $< -o $@

$(BUILD)/tests/test_%.o: $(MEISO_PUBLIC_ROOT_RESOLVED)/tests/test_%.c | $(BUILD)/tests
	@$(ECHO) "[CC] $@"
	$(Q)$(CC) $(CPPFLAGS) $(CFLAGS) \
		-Dmain=meiso_conformance_run_$* \
		-DMEISO_TEST_TARGET_STATIC_STORAGE=1 \
		-MMD -MP -MF $(@:.o=.d) -c $< -o $@

$(MEISO_RUNNER_OBJ): $(MEISO_PUBLIC_ROOT_RESOLVED)/targets/conformance/main.c | $(BUILD)/runner
	@$(ECHO) "[CC] $@"
	$(Q)$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -MF $(@:.o=.d) -c $< -o $@

$(BUILD)/core $(BUILD)/tests $(BUILD)/runner $(BIN_DIR):
	$(Q)mkdir -p $@

clean:
	$(Q)$(RM) $(BIN) $(BUILD)/$(DIR).map

distclean: clean
	$(Q)$(RM) $(BUILD)

-include $(DEPS)
