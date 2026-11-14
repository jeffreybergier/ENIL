# Shared native LTSM library and offline harness. Only static linkage is needed;
# both apps keep their existing Core linkage. Generated LINE code stays in build/.
LTSM_DIR = $(PROJECT_ROOT)/source/ltsm
LTSM_GEN = $(BUILD_ROOT)/ltsm-generated
LTSM_TRANSLATOR = $(PROJECT_ROOT)/source/deps/w2c2/w2c2
ENIL_LOCAL_CRYPTO ?= 1
ifneq ($(filter ENIL,$(PRODUCTS)),)
ENIL_FLAGS += -DENIL_LOCAL_CRYPTO=$(ENIL_LOCAL_CRYPTO) -I$(LTSM_DIR)
ifeq ($(ENIL_LOCAL_CRYPTO),1)
ENIL_DEPS += ENILLTSM
ENIL_RESOURCE_MAP += $(PROJECT_ROOT)/source/deps/w2c2/LICENSE=Licenses/w2c2.txt
endif
endif
# Do not even generate/link native products for an explicitly remote app build.
# Standalone host tests always exercise the library, regardless of that switch.
LTSM_ENABLED = $(if $(filter ENIL,$(PRODUCTS)),$(ENIL_LOCAL_CRYPTO),1)
ifeq ($(LTSM_ENABLED),1)
LTSM_PARTS = 0000000000 0000000001 0000000002 0000000003 0000000004 0000000005 0000000006
LTSM_C = $(LTSM_GEN)/ltsm.c $(addprefix $(LTSM_GEN)/m4_ltsm.s,$(addsuffix .c,$(LTSM_PARTS)))
PRODUCTS += ENILLTSM ltsm-test ltsm-check
ENILLTSM_TYPE = static
ENILLTSM_GROUPS = ltsm_bindings ltsm_service ltsm_generated
GROUP_ltsm_bindings_SOURCES = $(LTSM_DIR)/bridge.c
# The pinned imported ABI contains intentionally unused registration arguments.
GROUP_ltsm_bindings_FLAGS = -Wno-unused-parameter
GROUP_ltsm_service_SOURCES = $(LTSM_DIR)/service.c $(LTSM_DIR)/legy.c
GROUP_ltsm_generated_SOURCES = $(LTSM_C)
# w2c2 emits structural labels/temporaries even when a branch is unused.
GROUP_ltsm_generated_FLAGS = -Wno-unused-parameter -Wno-unused-variable \
  -Wno-unused-label
# Translated WASM deliberately writes scratch locals and reloads memory views.
# Retain all analyzer safety checks; omit only dead-store noise in generated C.
ifneq ($(filter _analyze,$(MAKECMDGOALS)),)
GROUP_ltsm_generated_FLAGS += -Xclang -analyzer-disable-checker -Xclang deadcode.DeadStores
endif
GROUP_ltsm_generated_FLAGS_x86_64 = -Wno-unused-but-set-variable -Wno-unused-but-set-parameter
GROUP_ltsm_generated_FLAGS_arm64 = $(GROUP_ltsm_generated_FLAGS_x86_64)
GROUP_ltsm_generated_FLAGS_armv7 = $(GROUP_ltsm_generated_FLAGS_x86_64)
GROUP_ltsm_generated_FLAGS_host = $(GROUP_ltsm_generated_FLAGS_x86_64)
# GCC 4.2 requires more than 12 GiB of RAM to optimize the WASM-converted C:
# i386 -O3 exhausted a 12 GiB Docker VM. Use -O0 only for generated legacy
# code; handwritten library code and the main ENIL app remain optimized.
GROUP_ltsm_generated_FLAGS_ppc = -O0
GROUP_ltsm_generated_FLAGS_i386 = $(GROUP_ltsm_generated_FLAGS_ppc)
ENILLTSM_FLAGS = -I$(LTSM_DIR) -I$(LTSM_GEN) -isystem $(LTSM_TRANSLATOR) \
  -DW2C2_RUNTIME_CHECKS=1 -Dtrap=enil_ltsm_trap
ENILLTSM_GENERATED = $(LTSM_GEN)/ltsm.c
ltsm-test_TYPE = executable
ltsm-test_SOURCES = $(LTSM_DIR)/test_cli.c
ltsm-test_FLAGS = -I$(LTSM_DIR)
ltsm-test_DEPS = ENILLTSM
ltsm-test_LDFLAGS = -lpthread -lm
ltsm-check_TYPE = executable
ltsm-check_SOURCES = $(LTSM_DIR)/test_api.c \
  $(PROJECT_ROOT)/source/shared/enil_worker.c \
  $(PROJECT_ROOT)/source/shared/enil_health.c \
  $(PROJECT_ROOT)/source/shared/enil_api_json.c
ltsm-check_FLAGS = -I$(LTSM_DIR) -I$(PROJECT_ROOT)/source/shared \
  -DENIL_LOCAL_CRYPTO=1 -D_DEFAULT_SOURCE
ltsm-check_DEPS = ENILLTSM
ltsm-check_LDFLAGS = -lpthread -lm
ENILLTSM_PACKAGE_RESOURCES = $(LTSM_DIR)/enil_ltsm.h=include/enil_ltsm.h \
  $(PROJECT_ROOT)/source/deps/w2c2/LICENSE=licenses/w2c2.txt
ifeq ($(LTSM_HOST),1)
ENILLTSM_FLAGS += $(shell pkg-config --cflags libcjson openssl)
ltsm-test_FLAGS += $(shell pkg-config --cflags libcjson openssl)
ltsm-test_LDFLAGS += $(shell pkg-config --libs libcjson openssl)
ltsm-check_FLAGS += $(shell pkg-config --cflags libcjson openssl libcurl)
ltsm-check_LDFLAGS += $(shell pkg-config --libs libcjson openssl)
else
ENILLTSM_CORE = static
ltsm-test_CORE = static
ltsm-check_CORE = static
endif
GENERATORS += enil_ltsm
GEN_enil_ltsm_OUTPUT = $(LTSM_GEN)/ltsm.c
GEN_enil_ltsm_INPUTS = $(LTSM_DIR)/generate.py \
  $(PROJECT_ROOT)/source/cloudflare/scripts/build.mjs \
  $(PROJECT_ROOT)/source/cloudflare/scripts/native-mids.mjs \
  $(PROJECT_ROOT)/source/cloudflare/scripts/extension.json \
  $(LTSM_TRANSLATOR)/w2c2_base.h
GEN_enil_ltsm_COMMAND = python3 $(LTSM_DIR)/generate.py --output $(LTSM_GEN)
# The single locked generator produces all seven translation units and headers.
$(filter-out $(LTSM_GEN)/ltsm.c,$(LTSM_C)): $(LTSM_GEN)/ltsm.c
endif
