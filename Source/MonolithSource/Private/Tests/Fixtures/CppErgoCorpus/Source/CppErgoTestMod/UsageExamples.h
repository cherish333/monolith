// Fixture header for VerifySymbolsComposition + FindExampleUsagePagination.
// NOT compiled into any module — read as data by the indexer during tests.
//
// UCppErgoUsage is indexed as a class symbol row. CallMe is a class-body method
// declaration (Step-0: NOT indexed as a symbols row) — verify_symbols must report
// it exists:true via the owning class row + source_fts declaration hit, NOT via
// symbols-table presence.
//
// NOTE: the opening brace MUST sit on the `class` declaration line (K&R). The
// non-UE class parser (MonolithCppParser.cpp NonUEPattern) anchors `^...class
// <Name> ... {` and requires a SAME-LINE `{`; an allman brace on the next line
// is not indexed as a class symbol row, which would break the class-existence
// assertions. The allman-class indexing gap is tracked as a deferred parser
// follow-up in the plan's "Considered and deferred" section.
#pragma once

class UCppErgoUsage {
public:
	void Run();

	void CallMe(int32 Value);
};
