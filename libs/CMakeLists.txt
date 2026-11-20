# One folder per library, listed in dependency order.
# Each library can only see the headers of the libraries it links.
add_subdirectory(core)        # types, Command/Event, EventSink, BookConfig
add_subdirectory(memory)      # Pool<T>
add_subdirectory(concurrency) # SpscRing<T>
add_subdirectory(reference)   # RefOrderBook: slow std::map oracle
add_subdirectory(book)        # OrderBook: the fast book
add_subdirectory(engine)      # Engine<Book>: instrument id -> book
add_subdirectory(journal)     # event log, FNV-1a hash sink
add_subdirectory(sim)         # deterministic load generator (runs a reference engine)
add_subdirectory(runtime)     # threaded gateway -> engine -> publisher pipeline
