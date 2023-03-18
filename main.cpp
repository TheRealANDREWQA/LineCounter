#include "ECSEngineUtilities.h"
#include "ECSEngineMultithreading.h"
#include "ECSEngineWorld.h"

#define SEARCH_PATH_FILE L"line_count.in"
#define OUTPUT_FILE L"line_count.out"
#define MAX_FILES (ECS_KB * 256)

#define MAX_NEW_LINES_PER_FILE ECS_KB * 128

#define PER_THREAD_ADDITIONAL_MESSAGE_CAPACITY ECS_MB * 2

#define MAIN_THREAD_SLEEP_TICK 15

using namespace ECSEngine;

// Returns true if there are any sloc characters
bool AreSlocCharacters(const char* first, const char* end) {
	const char* initial_end = end;

	first = function::SkipWhitespace(first);
	if (first > end) {
		return false;
	}
	Stream<char> line_contents = { first, function::PointerDifference(end, first) };

	while (first < end) {
		if (function::IsCodeIdentifierCharacter(first[0])) {
			return true;
		}
		first++;
	}

	return false;
}

// Returns -1 if there is a parsing error
size_t GetSloc(Stream<char> content, CapacityStream<unsigned int> new_line_positions) {
	// Get the new line count
	function::FindToken(content, '\n', new_line_positions);
	ECS_ASSERT(new_line_positions.size < MAX_NEW_LINES_PER_FILE, "Too many lines for a file.");

	size_t sloc_count = new_line_positions.size + 1;

	// For each line, verify its content
	unsigned int current_character = 0;
	unsigned int last_line_character_offset = 0;

	// Returns true if a parsing error has occured
	auto verify_line = [&]() {
		const char* last_line_character = content.buffer + last_line_character_offset;
		const char* first_char_non_space = function::SkipWhitespace(content.buffer + current_character);
		// If the first non space character is the same as the end of the line, then skip
		if (first_char_non_space == last_line_character) {
			sloc_count--;
			current_character = last_line_character_offset + 1;
			return;
		}

		// Check for non parenthese line
		bool has_sloc = AreSlocCharacters(first_char_non_space, last_line_character);
		sloc_count -= !has_sloc;
		current_character = last_line_character_offset + 1;
	};

	for (unsigned int index = 0; index < new_line_positions.size; index++) {
		last_line_character_offset = new_line_positions[index];
		verify_line();
	}

	last_line_character_offset = content.size;
	// The last line must be manually verified
	verify_line();

	return sloc_count;
}

struct LineCountThreadTaskData {
	AtomicStream<Stream<wchar_t>>* files;
	Stream<ThreadPartition> thread_partitions;
	std::atomic<size_t>* total_line_count;
	Semaphore* semaphore;
	// Per thread
	CapacityStream<char>** error_message;
	// Per thread
	CapacityStream<char>** additional_display_message;
	bool display_per_file_count;
};

ECS_THREAD_TASK(LineCountThreadTask) {
	LineCountThreadTaskData* data = (LineCountThreadTaskData*)_data;

	const size_t DEFAULT_BUFFER_SIZE = ECS_MB * 10;

	const unsigned int signal_initialization_finished = -2;
	unsigned int enter_index = data->semaphore->Exit();
	if (enter_index == world->task_manager->GetThreadCount() + 1 && data->semaphore->target.load(ECS_RELAXED) != signal_initialization_finished) {
		// Initialize the partitioning
		unsigned int partitioning_thread_count = ThreadPartitionStream(data->thread_partitions, data->files->size.load(ECS_RELAXED));
		// Signal all the other threads that it is done
		data->semaphore->target.store(signal_initialization_finished, ECS_RELAXED);
		data->semaphore->Enter(partitioning_thread_count - 1);
	}
	else {
		data->semaphore->SpinWait(-1, signal_initialization_finished);
	}

	if (data->thread_partitions[thread_id].size > 0) {
		// Allocate a default chunk of memory to read the whole file into memory
		Stream<char> file_buffer;
		file_buffer.buffer = (char*)malloc(sizeof(char) * DEFAULT_BUFFER_SIZE);
		file_buffer.size = DEFAULT_BUFFER_SIZE;
		CapacityStream<unsigned int> file_new_line_positions = { malloc(sizeof(unsigned int) * MAX_NEW_LINES_PER_FILE), 0, MAX_NEW_LINES_PER_FILE };

		ECS_FILE_HANDLE file_handle = 0;
		unsigned int offset = data->thread_partitions[thread_id].offset;

		ECS_FORMAT_STRING(*data->error_message[thread_id], "\nThread {#} errors:\n", thread_id);
		size_t errors = 0;

		size_t thread_sloc = 0;

		if (data->display_per_file_count) {
			ECS_FORMAT_STRING(*data->additional_display_message[thread_id], "\nThread {#} additional information:\n", thread_id);
		}

		for (unsigned int index = 0; index < data->thread_partitions[thread_id].size; index++) {
			Stream<wchar_t> current_path = data->files->buffer[offset + index];
			ECS_FILE_STATUS_FLAGS file_status = OpenFile(current_path, &file_handle, ECS_FILE_ACCESS_READ_ONLY | ECS_FILE_ACCESS_OPTIMIZE_SEQUENTIAL
				| ECS_FILE_ACCESS_TEXT, data->error_message[thread_id]);
			// If the opening succeded, try to read the whole file into a memory buffer
			if (file_status == ECS_FILE_STATUS_OK) {
				file_buffer.size = DEFAULT_BUFFER_SIZE;
				unsigned int bytes_read = ReadFromFile(file_handle, file_buffer);
				if (bytes_read == -1) {
					ECS_FORMAT_TEMP_STRING(temp_message, "Reading from {#} failed.\n", current_path);
					data->error_message[thread_id]->AddStreamSafe(temp_message);
					errors++;
				}
				else {
					file_buffer[bytes_read] = '\0';
					file_buffer.size = bytes_read;

					// Remove single and multi line comments
					file_buffer = function::RemoveSingleLineComment(file_buffer, ECS_C_FILE_SINGLE_LINE_COMMENT_TOKEN);
					file_buffer = function::RemoveMultiLineComments(file_buffer, ECS_C_FILE_MULTI_LINE_COMMENT_OPENED_TOKEN, ECS_C_FILE_MULTI_LINE_COMMENT_CLOSED_TOKEN);

					size_t sloc = GetSloc(file_buffer, file_new_line_positions);
					if (sloc == -1) {
						ECS_FORMAT_TEMP_STRING(temp_message, "Parsing {#} failed. Possible problems: invalid multi-line comments.\n", current_path);
						data->error_message[thread_id]->AddStreamSafe(temp_message);
					}
					else {
						thread_sloc += sloc;
						if (data->display_per_file_count) {
							ECS_FORMAT_TEMP_STRING(temp_message, "File {#} has {#} sloc.\n", current_path, sloc);
							data->additional_display_message[thread_id]->AddStreamSafe(temp_message);
						}
					}
				}

				// Close the file
				CloseFile(file_handle);
			}
			else {
				data->error_message[thread_id]->AddSafe('\n');
				errors++;
			}
		}
	
		if (errors == 0) {
			data->error_message[thread_id]->size = 0;
		}

		if (data->display_per_file_count) {
			ECS_FORMAT_STRING(*data->additional_display_message[thread_id], "Total line count for thread {#}.\n", thread_sloc);
		}

		free(file_buffer.buffer);
		free(file_new_line_positions.buffer);

		// Erroneous files will be excluded from the thread_sloc
		data->total_line_count->fetch_add(thread_sloc, ECS_RELAXED);
		data->semaphore->Exit();
	}

	ExitThread(0);
}

struct ListAllFilesInsidePathsData {
	AtomicStream<Stream<wchar_t>>* source_files;
	Stream<Stream<wchar_t>> search_paths;
	Stream<ThreadPartition> thread_partitions;
	Semaphore* semaphore;
};

ECS_THREAD_TASK(ListAllFilesInsidePaths) {
	ListAllFilesInsidePathsData* data = (ListAllFilesInsidePathsData*)_data;

	if (data->thread_partitions[thread_id].size > 0) {
		Stream<wchar_t> valid_extensions[] = {
			L".cpp",
			L".c",
			L".hpp",
			L".h"
		};
		
		struct FunctorData {
			ListAllFilesInsidePathsData* data;
			TaskManager* task_manager;
			unsigned int thread_id;
		};

		FunctorData functor_data = { data, world->task_manager, thread_id };

		for (size_t index = 0; index < data->thread_partitions[thread_id].size; index++) {
			ForEachFileInDirectoryRecursiveWithExtension(
				data->search_paths[data->thread_partitions[thread_id].offset + index],
				{ valid_extensions, std::size(valid_extensions) },
				&functor_data,
				[](Stream<wchar_t> path, void* _data) {
					FunctorData* data = (FunctorData*)_data;
					unsigned int position = data->data->source_files->RequestInt(1);
					data->data->source_files->buffer[position] = function::StringCopy(data->task_manager->GetThreadTempAllocator(data->thread_id), path);
					
					data->data->source_files->FinishRequest(1);

					return true;
				}
			);
		}
	}
	data->semaphore->Exit();
	data->semaphore->target.fetch_add(1, ECS_RELAXED);
	// We could use a tick wait, but a spin wait will provide the best results, plus this shouldn't take a lot of time
	SpinWait<'<'>(data->semaphore->target, world->task_manager->GetThreadCount());
}

int main(int argc, char** argv) {
	Timer timer;
	const size_t MAIN_THREAD_TICK_WAIT = 10;

	unsigned int thread_count = std::thread::hardware_concurrency();

	unsigned int search_paths_count = 0;
	Stream<Stream<wchar_t>> search_paths;
	GlobalMemoryManager global_memory(ECS_MB * thread_count + ECS_MB * 16, 1024, ECS_MB);

	bool display_per_file_sloc = true;

	// No command line arguments
	if (argc == 1) {
		// Use just the search path file
		Stream<char> file_content = ReadWholeFileText(SEARCH_PATH_FILE);
		if (file_content.buffer == nullptr) {
			printf("Could not open search file.\n");
			exit(1);
		}

		// Read each relative path
		ECS_STACK_CAPACITY_STREAM(unsigned int, new_line_positions, 1024);
		function::FindToken(file_content.buffer, '\n', new_line_positions);

		search_paths_count = new_line_positions.size;
		search_paths = { global_memory.Allocate(sizeof(Stream<wchar_t>) * search_paths_count), 0 };

		size_t starting_position = 0;
		for (size_t index = 0; index < search_paths_count; index++) {
			const char* whitespace = function::SkipWhitespace(file_content.buffer + starting_position + 1);
			if (whitespace == file_content.buffer + new_line_positions[index]) {
				starting_position = new_line_positions[index] + 1;
				continue;
			}

			// Make the new line '\0' and the transform into wchar_t
			file_content[new_line_positions[index]] = '\0';

			size_t character_count = new_line_positions[index] - starting_position + 1;
			void* allocation = global_memory.Allocate(sizeof(wchar_t) * character_count);
			CapacityStream<wchar_t> temp_stream(allocation, 0, character_count);

			function::ConvertASCIIToWide(temp_stream, { file_content.buffer + starting_position, (unsigned int)character_count, (unsigned int)character_count });
			search_paths.Add({ allocation, character_count });
			starting_position = new_line_positions[index] + 1;
		}
	}
	else {
		// Use the command line arguments
	}

	// Spawn a task manager
	TaskManager task_manager(thread_count, &global_memory, ECS_MB, 100);

	void* total_file_allocation = global_memory.Allocate(sizeof(Stream<wchar_t>) * MAX_FILES);
	AtomicStream<Stream<wchar_t>> source_files = AtomicStream<Stream<wchar_t>>(total_file_allocation, 0, MAX_FILES);;

	Semaphore semaphore_barrier;

	ListAllFilesInsidePathsData list_data;
	list_data.source_files = &source_files;
	list_data.thread_partitions = { global_memory.Allocate(sizeof(uint2) * thread_count), thread_count };
	list_data.search_paths = search_paths;
	list_data.semaphore = &semaphore_barrier;

	ThreadPartitionStream(list_data.thread_partitions, search_paths_count);
	semaphore_barrier.Enter(thread_count * 2 + 1);
	semaphore_barrier.ClearTarget();

	// Add the search tasks and then the count tasks
	ThreadTask list_task = ECS_THREAD_TASK_NAME(ListAllFilesInsidePaths, &list_data, sizeof(list_data));

	const size_t COUNT_ERROR_MESSAGE_CAPACITY = ECS_KB * 16;
	const size_t ADDITIONAL_MESSAGE_ALLOCATION_CAPACITY = ECS_KB * 64;

	// Allocate the capacity streams separately in order to avoid false sharing
	void* message_allocation = ECS_STACK_ALLOC(ECS_CACHE_LINE_SIZE * thread_count * 2 + ECS_CACHE_LINE_SIZE);
	message_allocation = function::AlignPointer(message_allocation, ECS_CACHE_LINE_SIZE);
	ECS_STACK_CAPACITY_STREAM_DYNAMIC(CapacityStream<char>*, per_thread_error_message, thread_count);
	ECS_STACK_CAPACITY_STREAM_DYNAMIC(CapacityStream<char>*, per_thread_additional_message, thread_count);

	for (unsigned int index = 0; index < thread_count; index++) {
		per_thread_error_message[index] = (CapacityStream<char>*)message_allocation;
		message_allocation = function::OffsetPointer(message_allocation, ECS_CACHE_LINE_SIZE);
		*per_thread_error_message[index] = { task_manager.AllocateTempBuffer(index, COUNT_ERROR_MESSAGE_CAPACITY), 0, COUNT_ERROR_MESSAGE_CAPACITY };

		per_thread_additional_message[index] = (CapacityStream<char>*)message_allocation;
		message_allocation = function::OffsetPointer(message_allocation, ECS_CACHE_LINE_SIZE);
		if (display_per_file_sloc) {
			*per_thread_additional_message[index] = { task_manager.AllocateTempBuffer(index, ADDITIONAL_MESSAGE_ALLOCATION_CAPACITY), 0, ADDITIONAL_MESSAGE_ALLOCATION_CAPACITY };
		}
		else {
			*per_thread_additional_message[index] = { nullptr, 0, 0 };
		}
	}

	std::atomic<size_t> total_line_count = 0;

	LineCountThreadTaskData count_data;
	count_data.files = &source_files;
	count_data.thread_partitions = { malloc(sizeof(uint2) * thread_count), thread_count };
	count_data.total_line_count = &total_line_count;
	count_data.semaphore = &semaphore_barrier;
	count_data.error_message = per_thread_error_message.buffer;
	count_data.additional_display_message = per_thread_additional_message.buffer;
	count_data.display_per_file_count = display_per_file_sloc;

	ThreadTask count_task = ECS_THREAD_TASK_NAME(LineCountThreadTask, &count_data, sizeof(count_data));
	World world;
	world.task_manager = &task_manager;

	task_manager.SetWorld(&world);

	task_manager.AddDynamicTaskGroup(list_task.function, list_task.name.buffer, &list_data, thread_count, sizeof(list_data));
	task_manager.AddDynamicTaskGroup(count_task.function, count_task.name.buffer, &count_data, thread_count, sizeof(count_data));

	task_manager.CreateThreads();

	// use a small tick wait for the signaling of the worked that has finished
	count_data.semaphore->TickWait(MAIN_THREAD_SLEEP_TICK, 0);
	ECS_STACK_CAPACITY_STREAM(char, line_message, 512);

	size_t microseconds_needed = timer.GetDurationSinceMarker(ECS_TIMER_DURATION_US);
	size_t milliseconds_needed = microseconds_needed / 1000;
	size_t seconds_needed = milliseconds_needed / 1000;
	ECS_FORMAT_STRING(line_message, "There are {#} lines.\nExecution time: {#} us - {#} ms - {#} s\n", count_data.total_line_count->load(ECS_RELAXED),
		microseconds_needed, milliseconds_needed, seconds_needed);
	printf("%s", line_message.buffer);

	for (unsigned int index = 0; index < thread_count; index++) {
		if (per_thread_error_message[index]->size > 0) {
			per_thread_error_message[index]->buffer[per_thread_error_message[index]->size] = '\0';
			printf("%s\n\n", per_thread_error_message[index]->buffer);
		}

		if (per_thread_additional_message[index]->size > 0) {
			per_thread_additional_message[index]->buffer[per_thread_additional_message[index]->size] = '\0';
			printf("%s\n\n", per_thread_additional_message[index]->buffer);
		}
	}

	ECS_FILE_HANDLE output_file = 0;
	ECS_FILE_STATUS_FLAGS output_status = FileCreate(OUTPUT_FILE, &output_file, ECS_FILE_ACCESS_WRITE_ONLY | ECS_FILE_ACCESS_TRUNCATE_FILE | ECS_FILE_ACCESS_TEXT);
	if (output_status != ECS_FILE_STATUS_OK) {
		printf("Could not create output file.");
	}
	else {
		if (!WriteFile(output_file, line_message)) {
			printf("Writing into output file line message failed.\n");
		}

		for (unsigned int index = 0; index < thread_count; index++) {
			if (per_thread_error_message[index]->size > 0) {
				if (!WriteFile(output_file, *per_thread_error_message[index])) {
					printf("Writing into output file error message failed.\n");
				}
			}

			if (per_thread_additional_message[index]->size > 0) {
				if (!WriteFile(output_file, { *per_thread_additional_message[index] })) {
					printf("Writing into output file additional thread messages failed.\n");
				}
			}
		}

		CloseFile(output_file);
	}

	return 0;
}