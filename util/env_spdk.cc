#include "env_posix.cc"

extern "C" {
#include "rte_log.h"
#include "spdk/env.h"
#include "spdk/event.h"
#include "spdk/blob.h"
#include "spdk/file.h"
#include "spdk/blob_bdev.h"
#include "spdk/log.h"
#include "spdk/io_channel.h"
#include "spdk/bdev.h"
}

namespace rocksdb {

struct spdk_filesystem *g_fs;
struct spdk_io_channel *g_channel;
struct spdk_bs_dev *g_bs_dev;
volatile bool g_spdk_ready = false;
struct sync_args {
	pthread_mutex_t mutex;
	pthread_cond_t cond;
	int rc;
	struct spdk_file *file;
	struct spdk_file_cache *cache;
	const char *new_name;
	const char *old_name;
	uint64_t size;
	void *buf;
	uint64_t offset;
	uint64_t len;
	std::vector<std::string>* children;
	uint32_t open_flags;
};

__thread struct sync_args g_sync_args;

static void
wake_rocksdb_thread(struct sync_args *args)
{
	pthread_mutex_lock(&args->mutex);
	pthread_cond_signal(&args->cond);
	pthread_mutex_unlock(&args->mutex);
}

static void
send_fs_event(spdk_event_fn fn, struct sync_args *args)
{
	struct spdk_event *event;

	args->rc = 0;
	event = spdk_event_allocate(0, fn, args, NULL);
	pthread_mutex_lock(&args->mutex);
	spdk_event_call(event);
	pthread_cond_wait(&args->cond, &args->mutex);
	pthread_mutex_unlock(&args->mutex);
}

static void
__call_fn(void *arg1, void *arg2)
{
	file_cache_request_fn fn;
	struct spdk_file_cache_args *args;

	fn = (file_cache_request_fn)arg1;
	args = (struct spdk_file_cache_args *)arg2;
	fn(args);
}

static void
__send_request(file_cache_request_fn fn, struct spdk_file_cache_args *args)
{
	struct spdk_event *event;

	event = spdk_event_allocate(0, __call_fn, (void *)fn, args);
	spdk_event_call(event);
}

class SpdkSequentialFile : public SequentialFile {
	struct spdk_file *mFile;
	struct spdk_file_cache *mFileCache;
	uint64_t mOffset;
	uint64_t mSize;
	char *mName;
public:
	SpdkSequentialFile(const std::string& fname, const EnvOptions& options);
	virtual ~SpdkSequentialFile();

	virtual Status Read(size_t n, Slice* result, char *scratch) override;
	virtual Status Skip(uint64_t n) override;
	virtual Status InvalidateCache(size_t offset, size_t length) override;
};

static void
__open_file_cb(void *arg, struct spdk_file *f, int fserrno)
{
	struct sync_args *args = (struct sync_args *)arg;

	assert(fserrno == 0);

	args->file = f;
	args->size = spdk_file_md_get_length(f);
	wake_rocksdb_thread(args);
}

static void
__open_file(void *arg1, void *arg2)
{
	struct sync_args *args = (struct sync_args *)arg1;

	spdk_fs_md_open_file(g_fs, args->new_name, args->open_flags, __open_file_cb, args);
}

static void
__delete_file_cb(void *arg, int fserrno)
{
	struct sync_args *args = (struct sync_args *)arg;

	if (fserrno != 0) {
		printf("could not delete %d\n", fserrno);
	}
	args->rc = fserrno;
	wake_rocksdb_thread(args);
}

static void
__delete_file(void *arg1, void *arg2)
{
	struct sync_args *args = (struct sync_args *)arg1;

	spdk_fs_md_delete_file(g_fs, args->new_name, __delete_file_cb, args);
}

static void
__rename_file_cb(void *arg, int fserrno)
{
	struct sync_args *args = (struct sync_args *)arg;

	args->rc = fserrno;
	wake_rocksdb_thread(args);
}

static void
__rename_file(void *arg1, void *arg2)
{
	struct sync_args *args = (struct sync_args *)arg1;

	spdk_fs_md_rename_file(g_fs, args->old_name, args->new_name, __rename_file_cb, args);
}

static void
__get_file(void *arg1, void *arg2)
{
	struct sync_args *args = (struct sync_args *)arg1;
	struct spdk_file *file;
	spdk_fs_iter iter;

	iter = spdk_fs_md_iter_first(g_fs);
	while (iter != NULL) {
		file = spdk_fs_iter_get_file(iter);
		iter = spdk_fs_md_iter_next(iter);
		if (!strcmp(args->new_name, spdk_file_md_get_name(file))) {
			args->file = file;
			args->size = spdk_file_md_get_length(file);
			args->rc = 0;
			wake_rocksdb_thread(args);
			return;
		}
	}

	args->rc = -ENOENT;
	wake_rocksdb_thread(args);
}

static void
__truncate_cb(void *ctx, int fserrno)
{
	struct sync_args *args = (struct sync_args *)ctx;

	assert(fserrno == 0);
	wake_rocksdb_thread(args);
}

static void
__truncate(void *arg1, void *arg2)
{
	struct sync_args *args = (struct sync_args *)arg1;

	spdk_file_md_truncate(args->file, args->size, __truncate_cb, args);
}

static void
__close_cb(void *ctx, int fserrno)
{
	struct sync_args *args = (struct sync_args *)ctx;

	assert(fserrno == 0);
	wake_rocksdb_thread(args);
}

static void
__close(void *arg1, void *arg2)
{
	struct sync_args *args = (struct sync_args *)arg1;

	spdk_file_md_close(args->file, __close_cb, args);
}

static void
__rw_done(void *ctx, int fserrno)
{
	struct sync_args *args = (struct sync_args *)ctx;

	assert(fserrno == 0);
	wake_rocksdb_thread(args);
}

static void
__read(void *arg1, void *arg2)
{
	struct sync_args *args = (struct sync_args *)arg1;

	spdk_file_read(args->file, g_channel, args->buf, args->offset, args->len, __rw_done, args);
}

static void
__get_children(void *arg1, void *arg2)
{
	struct sync_args *args = (struct sync_args *)arg1;
	spdk_fs_iter iter;
	struct spdk_file *file;

	iter = spdk_fs_md_iter_first(g_fs);
	while (iter != NULL) {
		file = spdk_fs_iter_get_file(iter);
		args->children->push_back(std::string(spdk_file_md_get_name(file)));
		iter = spdk_fs_md_iter_next(iter);
	}
	wake_rocksdb_thread(args);
}

static std::string
basename(std::string full)
{
	return full.substr(full.rfind("/") + 1);
}

SpdkSequentialFile::SpdkSequentialFile(const std::string &fname, const EnvOptions& options) : mOffset(0) {
	mFileCache = spdk_file_cache_open_read(g_fs, fname.c_str());
	if (mFileCache != NULL) {
		return;
	}
	g_sync_args.new_name = fname.c_str();
	g_sync_args.open_flags = 0;
	send_fs_event(__open_file, &g_sync_args);
	mFile = g_sync_args.file;
	mSize = g_sync_args.size;
	mName = strdup(fname.c_str());
}

SpdkSequentialFile::~SpdkSequentialFile(void) {
	if (mFileCache != NULL) {
		spdk_file_cache_close_read(mFileCache);
		return;
	}
	g_sync_args.file = mFile;
	send_fs_event(__close, &g_sync_args);
	free(mName);
}

Status
SpdkSequentialFile::Read(size_t n, Slice *result, char *scratch) {
	uint64_t ret;

	if (mFileCache != NULL) {
		ret = spdk_file_cache_read(mFileCache, scratch, mOffset, n,
					   &g_sync_args.mutex, &g_sync_args.cond,
					   g_channel);
		mOffset += ret;
		*result = Slice(scratch, ret);
		return Status::OK();
	}
	if (mOffset + n > mSize) {
		n = mSize - mOffset;
	}
	if (n == 0) {
		*result = Slice(scratch, 0);
		return Status::OK();
	}
	g_sync_args.file = mFile;
	g_sync_args.buf = scratch;
	g_sync_args.offset = mOffset;
	g_sync_args.len = n;
	send_fs_event(__read, &g_sync_args);
	mOffset += n;
	*result = Slice(scratch, n);
	return Status::OK();
}

Status
SpdkSequentialFile::Skip(uint64_t n) {
	mOffset += n;
	return Status::OK();
}

Status
SpdkSequentialFile::InvalidateCache(size_t offset, size_t length) {
	printf("SpdkSequentialFile::InvalidateCache()\n");
	return Status::OK();
}

class SpdkRandomAccessFile : public RandomAccessFile {
	struct spdk_file *mFile;
	struct spdk_file_cache *mFileCache;
public:
	SpdkRandomAccessFile(const std::string& fname, const EnvOptions& options);
	virtual ~SpdkRandomAccessFile();

	virtual Status Read(uint64_t offset, size_t n, Slice* result, char *scratch) const override;
	virtual Status InvalidateCache(size_t offset, size_t length) override;
};

SpdkRandomAccessFile::SpdkRandomAccessFile(const std::string &fname, const EnvOptions& options) {
	mFileCache = spdk_file_cache_open_read(g_fs, fname.c_str());
	if (mFileCache != NULL) {
		return;
	}
	g_sync_args.new_name = fname.c_str();
	g_sync_args.open_flags = 0;
	send_fs_event(__open_file, &g_sync_args);
	mFile = g_sync_args.file;
}

SpdkRandomAccessFile::~SpdkRandomAccessFile(void) {
	if (mFileCache != NULL) {
		spdk_file_cache_close_read(mFileCache);
		return;
	}
	g_sync_args.file = mFile;
	send_fs_event(__close, &g_sync_args);
}

Status
SpdkRandomAccessFile::Read(uint64_t offset, size_t n, Slice *result, char *scratch) const {
	if (mFileCache != NULL) {
		spdk_file_cache_read(mFileCache, scratch, offset, n,
				     &g_sync_args.mutex, &g_sync_args.cond,
				     g_channel);
		*result = Slice(scratch, n);
		return Status::OK();
	}
	g_sync_args.file = mFile;
	g_sync_args.buf = scratch;
	g_sync_args.offset = offset;
	g_sync_args.len = n;
	send_fs_event(__read, &g_sync_args);
	*result = Slice(scratch, n);
	return Status::OK();
}

Status
SpdkRandomAccessFile::InvalidateCache(size_t offset, size_t length) {
	printf("SpdkRandomAccessFile::InvalidateCache()\n");
	return Status::OK();
}

class SpdkWritableFile : public WritableFile {
	struct spdk_file_cache *mFile;
	uint32_t mSize;
	char *mName;

public:
	SpdkWritableFile(const std::string& fname, const EnvOptions& options);
	~SpdkWritableFile() {
		if (mFile != NULL) {
			Close();
		}
		free(mName);
	}

	virtual void SetIOPriority(Env::IOPriority pri) {
		if (pri == Env::IO_HIGH) {
			spdk_file_cache_set_priority(mFile, SPDK_FILE_CACHE_PRIORITY_HIGH);
		}
	}

	virtual Status Truncate(uint64_t size) override {
		g_sync_args.file = spdk_file_cache_get_file(mFile);
		g_sync_args.size = size;
		send_fs_event(__truncate, &g_sync_args);
		mSize = size;
		return Status::OK();
	}
	virtual Status Close() override {
		spdk_file_cache_sync(mFile, &g_sync_args.mutex, &g_sync_args.cond, g_channel);
		spdk_file_cache_close(mFile, &g_sync_args.mutex, &g_sync_args.cond);
		mFile = NULL;
		return Status::OK();
	}
	virtual Status Append(const Slice& data) override;
	virtual Status Flush() override {
		return Status::OK();
	}
	virtual Status Sync() override {
		spdk_file_cache_sync(mFile, &g_sync_args.mutex, &g_sync_args.cond, g_channel);
		return Status::OK();
	}
	virtual Status Fsync() override {
		spdk_file_cache_sync(mFile, &g_sync_args.mutex, &g_sync_args.cond, g_channel);
		return Status::OK();
	}
	virtual bool IsSyncThreadSafe() const override {
		printf("%s\n", __func__);
		return true;
	}
	virtual uint64_t GetFileSize() override { return mSize; }
	virtual Status InvalidateCache(size_t offset, size_t length) override {
		printf("%s\n", __func__);
		return Status::OK();
	}
#ifdef ROCKSDB_FALLOCATE_PRESENT
	virtual Status Allocate(uint64_t offset, uint64_t len) override {
		g_sync_args.file = spdk_file_cache_get_file(mFile);
		g_sync_args.size = offset + len;
		send_fs_event(__truncate, &g_sync_args);
		return Status::OK();
	}
	virtual Status RangeSync(uint64_t offset, uint64_t nbytes) override {
		printf("%s\n", __func__);
		return Status::OK();
	}
	virtual size_t GetUniqueId(char *id, size_t max_size) const override { return 0; }
#endif
};

SpdkWritableFile::SpdkWritableFile(const std::string &fname, const EnvOptions& options) : mSize(0) {
	mFile = spdk_file_cache_open(g_fs, fname.c_str(), __send_request,
				     &g_sync_args.mutex, &g_sync_args.cond);
	mName = strdup(fname.c_str());
}

Status
SpdkWritableFile::Append(const Slice& data) {
	spdk_file_cache_write(mFile, (void *)data.data(), mSize, data.size(), &g_sync_args.mutex, &g_sync_args.cond, g_channel);
	mSize += data.size();

	return Status::OK();
}

class SpdkDirectory : public Directory {
public:
	SpdkDirectory() {}
	~SpdkDirectory() {}
	Status Fsync() override {
		return Status::OK();
	}
};

class SpdkEnv : public PosixEnv {
private:
	pthread_t mSpdkTid;
	std::string mDirectory;
	std::string mConfig;

public:
	SpdkEnv(const std::string &dir, const std::string &conf, uint64_t cache_size_in_mb);

	virtual ~SpdkEnv();

	virtual Status NewSequentialFile(const std::string& fname,
                                   unique_ptr<SequentialFile>* result,
                                   const EnvOptions& options) override {
		if (fname.compare(0, mDirectory.length(), mDirectory) == 0) {
			std::string fname_base = basename(fname);
			g_sync_args.new_name = fname_base.c_str();
			send_fs_event(__get_file, &g_sync_args);
			if (g_sync_args.rc == 0) {
				result->reset(new SpdkSequentialFile(basename(fname), options));
				return Status::OK();
			} else {
				return IOError(fname, -g_sync_args.rc);
			}
		} else {
			return PosixEnv::NewSequentialFile(fname, result, options);
		}
	}

	virtual Status NewRandomAccessFile(const std::string& fname,
                                     unique_ptr<RandomAccessFile>* result,
                                     const EnvOptions& options) override {
		if (fname.compare(0, mDirectory.length(), mDirectory) == 0) {
			result->reset(new SpdkRandomAccessFile(basename(fname), options));
			return Status::OK();
		} else {
			return PosixEnv::NewRandomAccessFile(fname, result, options);
		}
	}

	virtual Status NewWritableFile(const std::string& fname,
                                 unique_ptr<WritableFile>* result,
                                 const EnvOptions& options) override {
		if (fname.compare(0, mDirectory.length(), mDirectory) == 0) {
			result->reset(new SpdkWritableFile(basename(fname), options));
			return Status::OK();
		} else {
			return PosixEnv::NewWritableFile(fname, result, options);
		}
	}

	virtual Status ReuseWritableFile(const std::string& fname,
                                   const std::string& old_fname,
                                   unique_ptr<WritableFile>* result,
                                   const EnvOptions& options) override {
		printf("%s: %s from %s\n", __func__, fname.c_str(), old_fname.c_str());
		return PosixEnv::ReuseWritableFile(fname, old_fname, result, options);
	}

	virtual Status NewDirectory(const std::string& name,
                              unique_ptr<Directory>* result) override {
		printf("%s: %s\n", __func__, name.c_str());
		result->reset(new SpdkDirectory());
		return Status::OK();
	}
	virtual Status FileExists(const std::string& fname) override {
		std::string fname_base = basename(fname);
		g_sync_args.new_name = fname_base.c_str();
		send_fs_event(__get_file, &g_sync_args);
		if (g_sync_args.rc == 0) {
			return Status::OK();
		}
		return PosixEnv::FileExists(fname);
	}
	virtual Status RenameFile(const std::string& src, const std::string& target) override {
		std::string target_base = basename(target);
		std::string src_base = basename(src);
		g_sync_args.new_name = target_base.c_str();
		g_sync_args.old_name = src_base.c_str();
		send_fs_event(__rename_file, &g_sync_args);

		if (g_sync_args.rc == -ENOENT) {
			return PosixEnv::RenameFile(src, target);
		}
		return Status::OK();
	}
	virtual Status GetFileSize(const std::string& fname, uint64_t* size) override {
		std::string fname_base = basename(fname);
		g_sync_args.new_name = fname_base.c_str();
		send_fs_event(__get_file, &g_sync_args);
		if (g_sync_args.rc == -ENOENT) {
			return PosixEnv::GetFileSize(fname, size);
		}
		*size = g_sync_args.size;
		return Status::OK();
	}
	virtual Status DeleteFile(const std::string& fname) override {
		std::string fname_base = basename(fname);
		g_sync_args.new_name = fname_base.c_str();
		send_fs_event(__delete_file, &g_sync_args);
		if (g_sync_args.rc == -ENOENT) {
			return PosixEnv::DeleteFile(fname);
		}
		return Status::OK();
	}
	virtual void StartThread(void (*function)(void *arg), void *arg) override;
	virtual Status LockFile(const std::string& fname, FileLock** lock) override {
		std::string fname_base = basename(fname);
		g_sync_args.new_name = fname_base.c_str();
		g_sync_args.open_flags = 0;
		send_fs_event(__open_file, &g_sync_args);
		*lock = (FileLock*)g_sync_args.file;
		return Status::OK();
		//return PosixEnv::LockFile(fname, lock);
	}
	virtual Status UnlockFile(FileLock* lock) override {
		g_sync_args.file = (struct spdk_file *)lock;
		send_fs_event(__close, &g_sync_args);
		return Status::OK();
		//return PosixEnv::UnlockFile(lock);
	}
	virtual Status GetChildren(const std::string& dir,
				   std::vector<std::string>* result) override {
		if (dir.find("archive") != std::string::npos) {
			return Status::OK();
		}
		if (dir.compare(0, mDirectory.length(), mDirectory) == 0) {
			g_sync_args.children = result;
			send_fs_event(__get_children, &g_sync_args);
			return Status::OK();
		}
		return PosixEnv::GetChildren(dir, result);
	}
};

void SpdkInitializeThread(void)
{
	pthread_mutex_init(&g_sync_args.mutex, NULL);
	pthread_cond_init(&g_sync_args.cond, NULL);
}

static void SpdkStartThreadWrapper(void* arg) {
	StartThreadState* state = reinterpret_cast<StartThreadState*>(arg);

	SpdkInitializeThread();
	StartThreadWrapper(state);
}

void SpdkEnv::StartThread(void (*function)(void* arg), void* arg)
{
  StartThreadState* state = new StartThreadState;
  state->user_function = function;
  state->arg = arg;
  PosixEnv::StartThread(SpdkStartThreadWrapper, state);
}

static void
fs_init_cb(void *ctx, struct spdk_filesystem *fs, int fserrno)
{
	assert(fserrno == 0);

	g_fs = fs;
	g_channel = spdk_fs_alloc_io_channel(g_fs, SPDK_IO_PRIORITY_DEFAULT);
	g_spdk_ready = true;
}

static void
spdk_rocksdb_run(void *arg1, void *arg2)
{
	struct spdk_bdev *bdev;

	pthread_setname_np(pthread_self(), "spdk");
	bdev = spdk_bdev_first();

	if (bdev == NULL) {
		printf("no bdevs found\n");
		exit(1);
	}

	g_bs_dev = spdk_bdev_create_bs_dev(bdev);
	printf("using bdev %s\n", bdev->name);
	spdk_fs_load(g_bs_dev, fs_init_cb, NULL);
}

static void
fs_unload_cb(void *ctx, int fserrno)
{
	assert(fserrno == 0);

	spdk_bdev_destroy_bs_dev(g_bs_dev);
	spdk_app_stop(0);
}

static void
spdk_rocksdb_shutdown(void)
{
	spdk_fs_free_io_channel(g_channel);
	spdk_fs_unload(g_fs, fs_unload_cb, NULL);
}

static void *
initialize_spdk(void *arg)
{
	struct spdk_app_opts *opts = (struct spdk_app_opts *)arg;

	rte_set_log_level(RTE_LOG_ERR);
	spdk_log_set_trace_flag("FILE");
	spdk_log_set_trace_flag("FILE_CACHE");
	spdk_app_init(opts);

	spdk_app_start(spdk_rocksdb_run, NULL, NULL);
	spdk_app_fini();

	delete opts;
	pthread_exit(NULL);
}

SpdkEnv::SpdkEnv(const std::string &dir, const std::string &conf, uint64_t cache_size_in_mb)
    : PosixEnv(), mDirectory(dir), mConfig(conf) {
	struct spdk_app_opts *opts = new struct spdk_app_opts;

	spdk_app_opts_init(opts);
	opts->name = "rocksdb";
	opts->config_file = mConfig.c_str();
	opts->reactor_mask = "0x1";
	opts->dpdk_mem_size = 4096 + cache_size_in_mb;
	opts->shutdown_cb = spdk_rocksdb_shutdown;

	spdk_file_cache_set_size(cache_size_in_mb);

	pthread_create(&mSpdkTid, NULL, &initialize_spdk, opts);
	while (!g_spdk_ready)
		;

	pthread_mutex_init(&g_sync_args.mutex, NULL);
	pthread_cond_init(&g_sync_args.cond, NULL);
}

SpdkEnv::~SpdkEnv() {
	spdk_app_start_shutdown();
	pthread_join(mSpdkTid, NULL);
}

void NewSpdkEnv(Env **env, const std::string& dir, const std::string &conf, uint64_t cache_size_in_mb) {
	*env = new SpdkEnv(dir, conf, cache_size_in_mb);
}

} // namespace rocksdb
