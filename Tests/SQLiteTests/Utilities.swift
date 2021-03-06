import Async
import Dispatch
import XCTest
@testable import SQLite

extension SQLiteConnection {
    static func makeTestConnection(queue: EventLoop) -> SQLiteConnection? {
        do {
            let sqlite = try SQLiteDatabase(storage: .memory)
            return try sqlite.makeConnection(on: queue).blockingAwait()
        } catch {
            XCTFail()
        }
        return nil
    }
}
