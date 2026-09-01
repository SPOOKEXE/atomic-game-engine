#include <engine/datastore/Sqlite.hpp>
#include <engine/world/SharedStoreFile.hpp>

#include <limits>
#include <span>
#include <sqlite3.h>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace engine::datastore {
	namespace {
		constexpr const char *CREATE_SCHEMA = "CREATE TABLE IF NOT EXISTS datastores ("
											  "name TEXT PRIMARY KEY NOT NULL, "
											  "image BLOB NOT NULL) WITHOUT ROWID";
		constexpr const char *LOAD_IMAGE = "SELECT image FROM datastores WHERE name = ?1";
		constexpr const char *SAVE_IMAGE = "INSERT INTO datastores(name, image) VALUES(?1, ?2) "
										   "ON CONFLICT(name) DO UPDATE SET image = excluded.image";

		class Database {
		  public:
			Database() = default;
			~Database() {
				if (Handle != nullptr) {
					sqlite3_close(Handle);
				}
			}

			Database(const Database &) = delete;
			Database &operator=(const Database &) = delete;

			sqlite3 *Handle = nullptr;
		};

		class Statement {
		  public:
			explicit Statement(sqlite3_stmt *statement = nullptr) : Handle(statement) {}
			~Statement() {
				if (Handle != nullptr) {
					sqlite3_finalize(Handle);
				}
			}

			Statement(const Statement &) = delete;
			Statement &operator=(const Statement &) = delete;

			sqlite3_stmt *Handle = nullptr;
		};

		world::DataStoreStatus FailureStatus(const int code) {
			switch (code & 0xff) {
			case SQLITE_CORRUPT:
			case SQLITE_NOTADB:
			case SQLITE_SCHEMA:
				return world::DataStoreStatus::Malformed;
			default:
				return world::DataStoreStatus::IoError;
			}
		}

		world::DataStoreStatus Fail(sqlite3 *database, const int code, std::string &error) {
			const char *message = database == nullptr ? sqlite3_errstr(code) : sqlite3_errmsg(database);
			error = "SQLite DataStore: ";
			error += message == nullptr ? "unknown error" : message;
			return FailureStatus(code);
		}

		bool ValidStoreName(const core::Name store) {
			return store.IsValid() && !store.Text().empty() &&
				   store.Text().size() <= world::MAXIMUM_DATASTORE_NAME_BYTES;
		}

		world::DataStoreStatus
		Open(const std::filesystem::path &path, const int flags, Database &database, std::string &error) {
			const int opened = sqlite3_open_v2(path.string().c_str(), &database.Handle, flags, nullptr);
			return opened == SQLITE_OK ? world::DataStoreStatus::Ok : Fail(database.Handle, opened, error);
		}

		world::DataStoreStatus
		Prepare(sqlite3 *database, const char *sql, Statement &statement, std::string &error) {
			sqlite3_stmt *prepared = nullptr;
			const int result = sqlite3_prepare_v2(database, sql, -1, &prepared, nullptr);
			if (result != SQLITE_OK) {
				return Fail(database, result, error);
			}
			statement.Handle = prepared;
			return world::DataStoreStatus::Ok;
		}

		class SqliteDataStoreAdapter final : public world::DataStoreAdapter {
		  public:
			SqliteDataStoreAdapter(
				std::filesystem::path root, const world::SharedStoreEnvironment environment
			)
				: Path(SqliteDataStorePath(root, environment)) {}

			world::DataStoreStatus Load(
				const core::Name store, std::vector<world::SharedStoreEntry> &entries, std::string &error
			) override {
				error.clear();
				if (!ValidStoreName(store)) {
					error = "invalid datastore name";
					return world::DataStoreStatus::Refused;
				}

				std::error_code statusError;
				if (!std::filesystem::exists(Path, statusError)) {
					if (statusError) {
						error = "could not inspect " + Path.string();
						return world::DataStoreStatus::IoError;
					}
					return world::DataStoreStatus::NotFound;
				}

				Database database;
				world::DataStoreStatus status = Open(Path, SQLITE_OPEN_READONLY, database, error);
				if (status != world::DataStoreStatus::Ok) {
					return status;
				}

				Statement statement;
				status = Prepare(database.Handle, LOAD_IMAGE, statement, error);
				if (status != world::DataStoreStatus::Ok) {
					return status;
				}
				if (sqlite3_bind_text(
						statement.Handle,
						1,
						store.Text().data(),
						static_cast<int>(store.Text().size()),
						SQLITE_TRANSIENT
					) != SQLITE_OK) {
					return Fail(database.Handle, sqlite3_errcode(database.Handle), error);
				}

				const int stepped = sqlite3_step(statement.Handle);
				if (stepped == SQLITE_DONE) {
					return world::DataStoreStatus::NotFound;
				}
				if (stepped != SQLITE_ROW) {
					return Fail(database.Handle, stepped, error);
				}
				if (sqlite3_column_type(statement.Handle, 0) != SQLITE_BLOB) {
					error = "SQLite DataStore image is not a blob";
					return world::DataStoreStatus::Malformed;
				}

				const int byteCount = sqlite3_column_bytes(statement.Handle, 0);
				if (byteCount < 0 ||
					static_cast<uint64_t>(byteCount) > world::MAXIMUM_SHARED_STORE_IMAGE_BYTES) {
					error = "SQLite DataStore image exceeds 256 MiB";
					return world::DataStoreStatus::Malformed;
				}
				const auto *first = static_cast<const std::byte *>(sqlite3_column_blob(statement.Handle, 0));
				if (byteCount > 0 && first == nullptr) {
					error = "SQLite DataStore image could not be read";
					return world::DataStoreStatus::IoError;
				}
				const std::span<const std::byte> image(first, static_cast<size_t>(byteCount));
				const world::SharedStoreFileStatus decoded =
					world::DecodeSharedStoreImage(image, world::BusKind::DataStore, entries, error);
				return decoded == world::SharedStoreFileStatus::Ok ? world::DataStoreStatus::Ok
																   : world::DataStoreStatus::Malformed;
			}

			world::DataStoreStatus Save(
				const core::Name store,
				const std::span<const world::SharedStoreEntry> entries,
				std::string &error
			) override {
				error.clear();
				if (!ValidStoreName(store)) {
					error = "invalid datastore name";
					return world::DataStoreStatus::Refused;
				}

				std::vector<std::byte> image;
				if (world::EncodeSharedStoreImage(world::BusKind::DataStore, entries, image, error) !=
					world::SharedStoreFileStatus::Ok) {
					return world::DataStoreStatus::Malformed;
				}
				if (image.size() > static_cast<size_t>(std::numeric_limits<int>::max())) {
					error = "SQLite DataStore image is too large";
					return world::DataStoreStatus::Malformed;
				}

				std::error_code directoryError;
				std::filesystem::create_directories(Path.parent_path(), directoryError);
				if (directoryError) {
					error = "could not create " + Path.parent_path().string();
					return world::DataStoreStatus::IoError;
				}

				Database database;
				world::DataStoreStatus status =
					Open(Path, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, database, error);
				if (status != world::DataStoreStatus::Ok) {
					return status;
				}
				const int schema = sqlite3_exec(database.Handle, CREATE_SCHEMA, nullptr, nullptr, nullptr);
				if (schema != SQLITE_OK) {
					return Fail(database.Handle, schema, error);
				}

				Statement statement;
				status = Prepare(database.Handle, SAVE_IMAGE, statement, error);
				if (status != world::DataStoreStatus::Ok) {
					return status;
				}
				const int nameBound = sqlite3_bind_text(
					statement.Handle,
					1,
					store.Text().data(),
					static_cast<int>(store.Text().size()),
					SQLITE_TRANSIENT
				);
				const int imageBound = sqlite3_bind_blob(
					statement.Handle, 2, image.data(), static_cast<int>(image.size()), SQLITE_TRANSIENT
				);
				if (nameBound != SQLITE_OK || imageBound != SQLITE_OK) {
					return Fail(database.Handle, sqlite3_errcode(database.Handle), error);
				}
				const int stepped = sqlite3_step(statement.Handle);
				return stepped == SQLITE_DONE ? world::DataStoreStatus::Ok
											  : Fail(database.Handle, stepped, error);
			}

		  private:
			std::filesystem::path Path;
		};
	}

	std::filesystem::path
	SqliteDataStorePath(const std::filesystem::path &root, const world::SharedStoreEnvironment environment) {
		return root / world::Describe(environment) / "datastores.sqlite3";
	}

	std::unique_ptr<world::DataStoreAdapter>
	MakeSqliteDataStoreAdapter(std::filesystem::path root, const world::SharedStoreEnvironment environment) {
		return std::make_unique<SqliteDataStoreAdapter>(std::move(root), environment);
	}
}
