// Copyright (c) 2019 Christoffer Lerno. All rights reserved.
// Use of this source code is governed by the GNU LGPLv3.0 license
// a copy of which can be found in the LICENSE file.

#include "compiler_internal.h"
#ifndef _MSC_VER
#include <unistd.h>
#endif

#define MAX_OUTPUT_FILES 1000000
#define MAX_MODULES 100000
GlobalContext global_context;
BuildTarget active_target;

Vmem ast_arena;
Vmem expr_arena;
Vmem sourceloc_arena;
Vmem toktype_arena;
Vmem tokdata_arena;
Vmem decl_arena;
Vmem type_info_arena;



void compiler_init(const char *std_lib_dir)
{
	DEBUG_LOG("Version: %s", COMPILER_VERSION);

	global_context = (GlobalContext ){ .in_panic_mode = false };
	// Skip library detection.
	//compiler.lib_dir = find_lib_dir();
	//DEBUG_LOG("Found std library: %s", compiler.lib_dir);
	stable_init(&global_context.modules, 64);
	stable_init(&global_context.compiler_defines, 512);
	global_context.module_list = NULL;
	global_context.generic_module_list = NULL;
	vmem_init(&ast_arena, 4 * 1024);
	vmem_init(&expr_arena, 4 * 1024);
	vmem_init(&decl_arena, 1024);
	vmem_init(&sourceloc_arena, 4 * 1024);
	vmem_init(&toktype_arena, 4 * 1024);
	vmem_init(&tokdata_arena, 4 * 1024);
	vmem_init(&type_info_arena, 1024);
	// Create zero index value.
	(void) sourceloc_calloc();
	(void) toktype_calloc();
	(void) tokdata_calloc();
	if (std_lib_dir)
	{
		global_context.lib_dir = std_lib_dir;
	}
	else
	{
		global_context.lib_dir = find_lib_dir();
	}
}

static void compiler_lex(void)
{
	VECEACH(global_context.sources, i)
	{
		bool loaded = false;
		File *file = source_file_load(global_context.sources[i], &loaded);
		if (loaded) continue;
		Lexer lexer = { .file = file };
		lexer_lex_file(&lexer);
		printf("# %s\n", file->full_path);
		uint32_t index = lexer.token_start_id;
		while (1)
		{
			TokenType token_type = (TokenType)(*toktypeptr(index));
			index++;
			printf("%s ", token_type_to_string(token_type));
			if (token_type == TOKEN_EOF) break;
		}
		printf("\n");
	}
	exit_compiler(COMPILER_SUCCESS_EXIT);
}

void compiler_parse(void)
{
	VECEACH(global_context.sources, i)
	{
		bool loaded = false;
		File *file = source_file_load(global_context.sources[i], &loaded);
		if (loaded) continue;

		global_context_clear_errors();
		parse_file(file);
	}
	exit_compiler(COMPILER_SUCCESS_EXIT);
}


typedef struct CompileData_
{
	void *context;
	const char *object_name;
	Task task;
} CompileData;

void thread_compile_task_llvm(void *compiledata)
{
	CompileData *data = compiledata;
	data->object_name = llvm_codegen(data->context);
}

void thread_compile_task_tb(void *compiledata)
{
	CompileData *data = compiledata;
	data->object_name = tinybackend_codegen(data->context);
}



static void add_global_define(const char *name, Expr *value)
{
	Decl *dec = decl_calloc();
	TokenType type = TOKEN_CONST_IDENT;
	const char *unique_name = symtab_add(name, (uint32_t)strlen(name), fnv1a(name, (uint32_t)strlen(name)), &type);
	dec->name = unique_name;
	dec->module = &global_context.std_module;
	dec->visibility = VISIBLE_PUBLIC;
	dec->decl_kind = DECL_VAR;
	dec->var.kind = VARDECL_CONST;
	dec->var.constant = true;
	dec->var.type_info = NULL;
	dec->var.init_expr = value;
	dec->type = value->type;
	dec->resolve_status = RESOLVE_DONE;
	decl_set_external_name(dec);
	stable_set(&dec->module->symbols, dec->name, dec);
}

static const char *active_target_name(void)
{
	if (active_target.name) return active_target.name;
	switch (active_target.arch_os_target)
	{
		case X86_WINDOWS:
		case X64_WINDOWS:
		case X64_WINDOWS_GNU:
			return "a.exe";
		default:
			return "a.out";
	}
}

static void free_arenas(void)
{
	if (debug_stats)
	{
		printf("-- AST/EXPR INFO -- \n");
		printf(" * Ast memory use: %llukb\n", (unsigned long long)ast_arena.allocated / 1024);
		printf(" * Decl memory use: %llukb\n", (unsigned long long)decl_arena.allocated / 1024);
		printf(" * Expr memory use: %llukb\n", (unsigned long long)expr_arena.allocated / 1024);
		printf(" * TypeInfo memory use: %llukb\n", (unsigned long long)type_info_arena.allocated / 1024);
		printf(" * Token memory use: %llukb\n", (unsigned long long)(toktype_arena.allocated) / 1024);
		printf(" * Sourceloc memory use: %llukb\n", (unsigned long long)(sourceloc_arena.allocated) / 1024);
		printf(" * Token data memory use: %llukb\n", (unsigned long long)(tokdata_arena.allocated) / 1024);
	}

	ast_arena_free();
	decl_arena_free();
	expr_arena_free();
	type_info_arena_free();
	sourceloc_arena_free();
	tokdata_arena_free();

	if (debug_stats) print_arena_status();
}

void compiler_compile(void)
{
	sema_analysis_run();

	Module **modules = global_context.module_list;
	unsigned module_count = vec_size(modules);

	if (module_count > MAX_MODULES)
	{
		error_exit("Too many modules.");
	}
	if (module_count < 1)
	{
		error_exit("No module to compile.");
	}

	if (active_target.output_headers)
	{
		for (unsigned i = 0; i < module_count; i++)
		{
			header_gen(modules[i]);
		}
		return;
	}

	if (active_target.check_only)
	{
		free_arenas();
		return;
	}

	void **gen_contexts = VECNEW(void*, module_count);
	void (*task)(void *);

	switch (active_target.backend)
	{
		case BACKEND_LLVM:
			llvm_codegen_setup();
			for (unsigned i = 0; i < module_count; i++)
			{
				void *result = llvm_gen(modules[i]);
				if (result) vec_add(gen_contexts, result);
			}
			task = &thread_compile_task_llvm;
			break;
		case BACKEND_TB:
			tinybackend_codegen_setup();
			for (unsigned i = 0; i < module_count; i++)
			{
				void *result = tinybackend_gen(modules[i]);
				if (result) vec_add(gen_contexts, result);
			}
			task = &thread_compile_task_tb;
			break;
		default:
			UNREACHABLE
	}


	free_arenas();


	bool create_exe = !active_target.no_link && !active_target.test_output && (active_target.type == TARGET_TYPE_EXECUTABLE || active_target.type == TARGET_TYPE_TEST);

	uint32_t output_file_count = vec_size(gen_contexts);
	if (output_file_count > MAX_OUTPUT_FILES)
	{
		error_exit("Too many output files.");
	}
	if (!output_file_count)
	{
		error_exit("No output files found.");
	}

	unsigned cfiles = vec_size(active_target.csources);
	CompileData *compile_data = ccalloc(sizeof(CompileData), output_file_count);
	const char **obj_files = cmalloc(sizeof(char*) * (output_file_count + cfiles));

	if (cfiles)
	{
		platform_compiler(active_target.csources, cfiles, active_target.cflags);
		for (int i = 0; i < cfiles; i++)
		{
			char *filename = NULL;
			char *dir = NULL;
			bool split_worked = filenamesplit(active_target.csources[i], &filename, NULL);
			assert(split_worked);
			size_t len = strlen(filename);
			// .c -> .o (quick hack to fix the name on linux)
			filename[len - 1] = 'o';
			obj_files[output_file_count + i] = filename;
		}

	}


	TaskQueueRef queue = taskqueue_create(16);


	for (unsigned i = 0; i < output_file_count; i++)
	{
		compile_data[i] = (CompileData) { .context = gen_contexts[i] };
		compile_data[i].task = (Task) { task, &compile_data[i] };
		taskqueue_add(queue, &compile_data[i].task);
	}

	taskqueue_wait_for_completion(queue);
	taskqueue_destroy(queue);

	for (unsigned i = 0; i < output_file_count; i++)
	{
		obj_files[i] = compile_data[i].object_name;
		assert(obj_files[i] || !create_exe);
	}

	output_file_count += cfiles;
	free(compile_data);

	if (create_exe)
	{
		const char *output_name = active_target_name();
		if (active_target.arch_os_target == ARCH_OS_TARGET_DEFAULT)
		{
			platform_linker(output_name, obj_files, output_file_count);
		}
		else
		{
			if (!obj_format_linking_supported(platform_target.object_format) || !linker(output_name, obj_files,
			                                                                            output_file_count))
			{
				printf("No linking is performed due to missing linker support.\n");
				active_target.run_after_compile = false;
			}
		}
		if (active_target.run_after_compile)
		{
			system(strformat("./%s", output_name));
		}
	}

	free(obj_files);
}

static const char **target_expand_source_names(const char** dirs, const char *suffix1, const char *suffix2, bool error_on_mismatch)
{
	const char **files = NULL;
	size_t len1 = strlen(suffix1);
	size_t len2 = strlen(suffix2);
	VECEACH(dirs, i)
	{
		const char *name = dirs[i];
		size_t name_len = strlen(name);
		if (name_len < 1) goto INVALID_NAME;
		if (name[name_len - 1] == '*')
		{
			if (name_len == 1 || name[name_len - 2] == '/')
			{
				char *path = copy_string(name, name_len - 1);
				file_add_wildcard_files(&files, path, false, suffix1, suffix2);
				continue;
			}
			if (name[name_len - 2] != '*') goto INVALID_NAME;
			if (name_len == 2 || name[name_len - 3] == '/')
			{
				char *path = copy_string(name, name_len - 2);
				file_add_wildcard_files(&files, path, true, suffix1, suffix2);
				continue;
			}
			goto INVALID_NAME;
		}
		if (name_len < 4) goto INVALID_NAME;
		if (strcmp(&name[name_len - len1], suffix1) != 0 &&
		    (name_len < 5 || strcmp(&name[name_len - len2], suffix2) != 0)) goto INVALID_NAME;
		vec_add(files, name);
		continue;
		INVALID_NAME:
		if (!error_on_mismatch) continue;
		error_exit("File names must end with %s or they cannot be compiled: '%s' is invalid.", name, suffix1);
	}
	return files;
}

void compile_target(BuildOptions *options)
{
	init_default_build_target(&active_target, options);
	compile();
}

void compile_file_list(BuildOptions *options)
{
	init_build_target(&active_target, options);
	compile();
}

void compile()
{
	active_target.sources = target_expand_source_names(active_target.source_dirs, ".c3", ".c3t", true);
	if (active_target.csource_dirs)
	{
		active_target.csources = target_expand_source_names(active_target.csource_dirs, ".c", ".c", false);
	}
	global_context.sources = active_target.sources;
	symtab_init(active_target.symtab_size ? active_target.symtab_size : 64 * 1024);
	target_setup(&active_target);

	if (!vec_size(active_target.sources)) error_exit("No files to compile.");
	if (active_target.lex_only)
	{
		compiler_lex();
		return;
	}
	if (active_target.parse_only)
	{
		compiler_parse();
		return;
	}
	compiler_compile();
}


void global_context_add_type(Type *type)
{
	DEBUG_LOG("Created type %s.", type->name);
	assert(type_ok(type));
	vec_add(global_context.type, type);
}

const char *get_object_extension(void)
{
	switch (active_target.arch_os_target)
	{
		case X64_WINDOWS:
		case X86_WINDOWS:
		case X64_WINDOWS_GNU:
			return ".obj";
		default:
			return ".o";
	}
}

Module *global_context_find_module(const char *name)
{
	return stable_get(&global_context.modules, name);
}

Module *compiler_find_or_create_module(Path *module_name, TokenId *parameters, bool is_private)
{
	Module *module = global_context_find_module(module_name->module);
	if (module) return module;

	DEBUG_LOG("Creating module %s.", module_name->module);
	// Set up the module.
	module = CALLOCS(Module);
	module->name = module_name;
	module->stage = ANALYSIS_NOT_BEGUN;
	module->parameters = parameters;
	module->is_generic = vec_size(parameters) > 0;
	module->is_private = is_private;
	stable_init(&module->symbols, 0x10000);
	stable_set(&global_context.modules, module_name->module, module);
	if (parameters)
	{
		vec_add(global_context.generic_module_list, module);
	}
	else
	{
		vec_add(global_context.module_list, module);
	}

	return module;
}

void scratch_buffer_clear(void)
{
	global_context.scratch_buffer_len = 0;
}

void scratch_buffer_append_len(const char *string, size_t len)
{
	if (len + global_context.scratch_buffer_len > MAX_STRING_BUFFER - 1)
	{
		error_exit("Scratch buffer size (%d chars) exceeded", MAX_STRING_BUFFER - 1);
	}
	memcpy(global_context.scratch_buffer + global_context.scratch_buffer_len, string, len);
	global_context.scratch_buffer_len += len;
}

void scratch_buffer_append(const char *string)
{
	scratch_buffer_append_len(string, strlen(string));
}

void scratch_buffer_append_signed_int(int64_t i)
{
	uint32_t len_needed = (uint32_t)sprintf(&global_context.scratch_buffer[global_context.scratch_buffer_len], "%lld", (long long)i);
	if (global_context.scratch_buffer_len + len_needed > MAX_STRING_BUFFER - 1)
	{
		error_exit("Scratch buffer size (%d chars) exceeded", MAX_STRING_BUFFER - 1);
	}
	global_context.scratch_buffer_len += len_needed;
}

void scratch_buffer_append_unsigned_int(uint64_t i)
{
	uint32_t len_needed = (uint32_t)sprintf(&global_context.scratch_buffer[global_context.scratch_buffer_len], "%llu", (unsigned long long)i);
	if (global_context.scratch_buffer_len + len_needed > MAX_STRING_BUFFER - 1)
	{
		error_exit("Scratch buffer size (%d chars) exceeded", MAX_STRING_BUFFER - 1);
	}
	global_context.scratch_buffer_len += len_needed;
}

void scratch_buffer_append_char(char c)
{
	if (global_context.scratch_buffer_len + 1 > MAX_STRING_BUFFER - 1)
	{
		error_exit("Scratch buffer size (%d chars) exceeded", MAX_STRING_BUFFER - 1);
	}
	global_context.scratch_buffer[global_context.scratch_buffer_len++] = c;
}

char *scratch_buffer_to_string(void)
{
	global_context.scratch_buffer[global_context.scratch_buffer_len] = '\0';
	return global_context.scratch_buffer;
}

const char *scratch_buffer_interned(void)
{
	TokenType type = TOKEN_INVALID_TOKEN;
	return symtab_add(global_context.scratch_buffer, global_context.scratch_buffer_len,
	                  fnv1a(global_context.scratch_buffer, global_context.scratch_buffer_len), &type);
}
