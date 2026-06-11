// SPDX-License-Identifier: MIT
// LLM C++ authoring ergonomics — Phase 1 automation tests (items 1-3).
// Plan: Plugins/Monolith/Docs/plans/2026-06-10-llm-cpp-ergonomics-actions.md (§12 Phase 1).
//
// Tests use disposable SQLite DBs at FPaths::AutomationTransientDir(), never the
// real EngineSource.db. Fixtures live under
// Source/MonolithSource/Private/Tests/Fixtures/CppErgoCorpus/.
//
// DEVIATION NOTE: the plan §12 names IMPLEMENT_CUSTOM_SIMPLE_AUTOMATION_TEST;
// this module's existing tests (and the sibling RI tests) use
// IMPLEMENT_SIMPLE_AUTOMATION_TEST, which is what compiles here. We match the
// in-tree idiom.

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "MonolithSourceActions.h"
#include "MonolithSourceDatabase.h"
#include "MonolithSourceIndexer.h"
#include "MonolithCursorCodec.h"

#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "SQLiteDatabase.h"

namespace MonolithCppErgoTestDetail
{
	/** Resolve the fixture corpus dir relative to the Monolith plugin install. */
	static FString GetFixtureCorpusDir()
	{
		TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("Monolith"));
		if (Plugin.IsValid())
		{
			return Plugin->GetBaseDir()
				/ TEXT("Source") / TEXT("MonolithSource")
				/ TEXT("Private") / TEXT("Tests") / TEXT("Fixtures") / TEXT("CppErgoCorpus");
		}
		return FPaths::ProjectPluginsDir()
			/ TEXT("Monolith") / TEXT("Source") / TEXT("MonolithSource")
			/ TEXT("Private") / TEXT("Tests") / TEXT("Fixtures") / TEXT("CppErgoCorpus");
	}

	/** A disposable temp DB path under AutomationTransientDir. */
	static FString MakeTempDbPath()
	{
		const FString Dir = FPaths::AutomationTransientDir();
		FPlatformFileManager::Get().GetPlatformFile().CreateDirectoryTree(*Dir);
		const FString Path = Dir / FString::Printf(TEXT("cppergo-test-%s.db"), *FGuid::NewGuid().ToString());
		IFileManager::Get().Delete(*Path, /*bRequireExists=*/false, /*bEvenReadOnly=*/true);
		return Path;
	}
}

// ---------------------------------------------------------------------------
// Test 1: DeprecationSchemaBootstrap — empty-DB CreateTablesIfNeeded() creates
// symbol_deprecations and stamps SchemaVersion 2.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCppErgoDeprecationSchemaBootstrapTest,
	"Monolith.Source.CppErgonomics.DeprecationSchemaBootstrap",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCppErgoDeprecationSchemaBootstrapTest::RunTest(const FString& /*Parameters*/)
{
	using namespace MonolithCppErgoTestDetail;

	const FString DbPath = MakeTempDbPath();
	{
		FMonolithSourceDatabase DB;
		TestTrue(TEXT("OpenForWriting"), DB.OpenForWriting(DbPath));
		TestTrue(TEXT("CreateTablesIfNeeded"), DB.CreateTablesIfNeeded());

		// schema_version meta == 2
		TestEqual(TEXT("schema_version stamped to 2"), DB.GetMeta(TEXT("schema_version")), FString(TEXT("2")));

		// Inserting a deprecation row succeeds (table exists) and counts.
		DB.InsertDeprecation(/*SymbolId=*/0, TEXT("Foo"), TEXT("5.4"), TEXT("Use Bar"), TEXT("UE_DEPRECATED"));
		TestEqual(TEXT("one deprecation row"), DB.GetDeprecationCount(), 1);

		TOptional<FMonolithDeprecationRow> Got = DB.GetDeprecation(TEXT("Foo"));
		TestTrue(TEXT("GetDeprecation returns a value"), Got.IsSet());
		if (Got.IsSet())
		{
			TestEqual(TEXT("version"), Got.GetValue().Version, FString(TEXT("5.4")));
			TestEqual(TEXT("message"), Got.GetValue().Message, FString(TEXT("Use Bar")));
			TestEqual(TEXT("kind"), Got.GetValue().Kind, FString(TEXT("UE_DEPRECATED")));
		}
		DB.Close();
	}
	IFileManager::Get().Delete(*DbPath, /*bRequireExists=*/false, /*bEvenReadOnly=*/true);
	return true;
}

// ---------------------------------------------------------------------------
// Test 2: DeprecationIndexExtraction — index the fixture corpus (project-only,
// no engine) and assert two rows with parsed names, version/message/kind, and
// symbol_id = NULL (class-body methods have no symbols row).
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCppErgoDeprecationIndexExtractionTest,
	"Monolith.Source.CppErgonomics.DeprecationIndexExtraction",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCppErgoDeprecationIndexExtractionTest::RunTest(const FString& /*Parameters*/)
{
	using namespace MonolithCppErgoTestDetail;

	const FString CorpusDir = GetFixtureCorpusDir();
	if (!IFileManager::Get().DirectoryExists(*CorpusDir))
	{
		AddError(FString::Printf(TEXT("Fixture corpus not found: %s"), *CorpusDir));
		return false;
	}

	const FString DbPath = MakeTempDbPath();

	// Index ONLY the fixture project corpus (engine source path empty -> skipped).
	{
		FMonolithSourceIndexer Indexer;
		Indexer.SetSourcePath(TEXT(""));            // skip engine phase
		Indexer.SetShaderPath(TEXT(""));
		Indexer.SetProjectPath(CorpusDir);          // ProjectPath/Source/* discovered
		Indexer.SetDatabasePath(DbPath);
		Indexer.SetCleanBuild(true);
		Indexer.SetIndexProjectSource(true);
		TestTrue(TEXT("RunSynchronous"), Indexer.RunSynchronous());
	}

	// Read back the rows.
	FMonolithSourceDatabase DB;
	if (!DB.Open(DbPath))
	{
		AddError(TEXT("Failed to reopen indexed DB"));
		IFileManager::Get().Delete(*DbPath, false, true);
		return false;
	}

	TestEqual(TEXT("two deprecation rows extracted"), DB.GetDeprecationCount(), 2);

	// Foo — UE_DEPRECATED(5.4, "Use Bar instead")
	TOptional<FMonolithDeprecationRow> Foo = DB.GetDeprecation(TEXT("Foo"));
	TestTrue(TEXT("Foo deprecated"), Foo.IsSet());
	if (Foo.IsSet())
	{
		TestEqual(TEXT("Foo version"), Foo.GetValue().Version, FString(TEXT("5.4")));
		TestEqual(TEXT("Foo message"), Foo.GetValue().Message, FString(TEXT("Use Bar instead")));
		TestEqual(TEXT("Foo kind"), Foo.GetValue().Kind, FString(TEXT("UE_DEPRECATED")));
	}

	// Baz — UE_DEPRECATED_FORGAME(5.5, "Baz is gone")
	TOptional<FMonolithDeprecationRow> Baz = DB.GetDeprecation(TEXT("Baz"));
	TestTrue(TEXT("Baz deprecated"), Baz.IsSet());
	if (Baz.IsSet())
	{
		TestEqual(TEXT("Baz version"), Baz.GetValue().Version, FString(TEXT("5.5")));
		TestEqual(TEXT("Baz message"), Baz.GetValue().Message, FString(TEXT("Baz is gone")));
		TestEqual(TEXT("Baz kind"), Baz.GetValue().Kind, FString(TEXT("UE_DEPRECATED_FORGAME")));
	}

	// StillFine must NOT be present.
	TestFalse(TEXT("StillFine not deprecated"), DB.GetDeprecation(TEXT("StillFine")).IsSet());

	// symbol_id NULL for both (class-body methods are not indexed as symbols).
	{
		FSQLiteDatabase* Raw = DB.GetRawHandle();
		if (Raw)
		{
			FSQLitePreparedStatement Stmt;
			Stmt.Create(*Raw, TEXT("SELECT COUNT(*) FROM symbol_deprecations WHERE symbol_id IS NULL;"));
			int32 NullCount = -1;
			if (Stmt.Step() == ESQLitePreparedStatementStepResult::Row)
			{
				int64 C = 0;
				Stmt.GetColumnValueByIndex(0, C);
				NullCount = static_cast<int32>(C);
			}
			TestEqual(TEXT("both rows have symbol_id NULL"), NullCount, 2);
		}
	}

	DB.Close();
	IFileManager::Get().Delete(*DbPath, false, true);
	return true;
}

// ---------------------------------------------------------------------------
// Test 3: IncludePathDerivation — pure unit over DeriveIncludePath.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCppErgoIncludePathDerivationTest,
	"Monolith.Source.CppErgonomics.IncludePathDerivation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCppErgoIncludePathDerivationTest::RunTest(const FString& /*Parameters*/)
{
	bool bIncludable = false;
	FString Warning;

	// Public/ -> strip, includable.
	{
		const FString In = TEXT("D:/Proj/Source/MyMod/Public/Sub/Thing.h");
		const FString Out = FMonolithSourceActions::DeriveIncludePath(In, bIncludable, Warning);
		TestEqual(TEXT("Public/ strips prefix"), Out, FString(TEXT("Sub/Thing.h")));
		TestTrue(TEXT("Public/ includable"), bIncludable);
		TestTrue(TEXT("Public/ no warning"), Warning.IsEmpty());
	}

	// Classes/ -> strip, includable.
	{
		const FString In = TEXT("D:/Proj/Source/MyMod/Classes/X.h");
		const FString Out = FMonolithSourceActions::DeriveIncludePath(In, bIncludable, Warning);
		TestEqual(TEXT("Classes/ strips prefix"), Out, FString(TEXT("X.h")));
		TestTrue(TEXT("Classes/ includable"), bIncludable);
	}

	// Internal/ -> strip, includable.
	{
		const FString In = TEXT("D:/Proj/Source/MyMod/Internal/Y.h");
		const FString Out = FMonolithSourceActions::DeriveIncludePath(In, bIncludable, Warning);
		TestEqual(TEXT("Internal/ strips prefix"), Out, FString(TEXT("Y.h")));
		TestTrue(TEXT("Internal/ includable"), bIncludable);
	}

	// Private/ -> NOT includable, same-module relative, warning names the module.
	{
		Warning.Empty();
		const FString In = TEXT("D:/Proj/Source/MyMod/Private/Z.h");
		const FString Out = FMonolithSourceActions::DeriveIncludePath(In, bIncludable, Warning);
		TestEqual(TEXT("Private/ same-module relative"), Out, FString(TEXT("Z.h")));
		TestFalse(TEXT("Private/ NOT includable"), bIncludable);
		TestTrue(TEXT("Private/ warning present"), Warning.Contains(TEXT("Private header")));
		TestTrue(TEXT("Private/ warning names module"), Warning.Contains(TEXT("MyMod")));
	}

	// Backslashes + no recognised prefix -> basename fallback.
	{
		Warning.Empty();
		const FString In = TEXT("C:\\Engine\\Source\\Runtime\\Core\\Foo\\Bar.h");
		const FString Out = FMonolithSourceActions::DeriveIncludePath(In, bIncludable, Warning);
		TestEqual(TEXT("no-prefix basename fallback"), Out, FString(TEXT("Bar.h")));
		TestTrue(TEXT("fallback includable"), bIncludable);
	}

	return true;
}

// ---------------------------------------------------------------------------
// Test 4: SignatureCompaction — CompactDeclaration strips inline bodies + macro
// continuations and joins multi-line declarations. No body leaks.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCppErgoSignatureCompactionTest,
	"Monolith.Source.CppErgonomics.SignatureCompaction",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCppErgoSignatureCompactionTest::RunTest(const FString& /*Parameters*/)
{
	using namespace MonolithCppErgoTestDetail;

	const FString FixturePath = GetFixtureCorpusDir() / TEXT("Signatures.h");
	TArray<FString> Lines;
	if (!FFileHelper::LoadFileToStringArray(Lines, *FixturePath))
	{
		AddError(FString::Printf(TEXT("Could not load signature fixture: %s"), *FixturePath));
		return false;
	}

	// Locate fixture declarations by content (line numbers are not assumed exact).
	int32 MultiIdx = INDEX_NONE, InlineIdx = INDEX_NONE, MacroIdx = INDEX_NONE;
	for (int32 i = 0; i < Lines.Num(); ++i)
	{
		if (Lines[i].Contains(TEXT("MultiLineDecl"))) MultiIdx = i;
		else if (Lines[i].Contains(TEXT("GetTransform()")) && Lines[i].Contains(TEXT("{"))) InlineIdx = i;
		else if (Lines[i].Contains(TEXT("void Thing(")))   MacroIdx = i;
	}

	TestTrue(TEXT("found MultiLineDecl"), MultiIdx != INDEX_NONE);
	TestTrue(TEXT("found inline GetTransform"), InlineIdx != INDEX_NONE);
	TestTrue(TEXT("found macro Thing"), MacroIdx != INDEX_NONE);

	if (MultiIdx != INDEX_NONE)
	{
		const FString Sig = FMonolithSourceActions::CompactDeclaration(Lines, MultiIdx);
		TestEqual(TEXT("multi-line joined"), Sig,
			FString(TEXT("float MultiLineDecl( int32 First, const FString& Second) const")));
		TestFalse(TEXT("multi-line no body"), Sig.Contains(TEXT("{")));
	}

	if (InlineIdx != INDEX_NONE)
	{
		const FString Sig = FMonolithSourceActions::CompactDeclaration(Lines, InlineIdx);
		// Body { return ... } must be cut.
		TestFalse(TEXT("inline body stripped"), Sig.Contains(TEXT("return")));
		TestFalse(TEXT("inline no brace"), Sig.Contains(TEXT("{")));
		TestTrue(TEXT("inline keeps signature"), Sig.Contains(TEXT("GetTransform()")));
	}

	if (MacroIdx != INDEX_NONE)
	{
		const FString Sig = FMonolithSourceActions::CompactDeclaration(Lines, MacroIdx);
		// The `void Thing(int32 x) { DoThing(x); }` line: body stripped.
		TestFalse(TEXT("macro body stripped"), Sig.Contains(TEXT("DoThing")));
		TestFalse(TEXT("macro no brace"), Sig.Contains(TEXT("{")));
	}

	return true;
}

// ---------------------------------------------------------------------------
// Phase 2 helper: index the fixture corpus into a disposable DB. Returns true
// on success; caller closes + deletes the DB.
//
// FIXTURE-COMPILE GUARD: the .cpp fixtures on disk carry a `.cpp.fixture`
// extension so UBT's `*.cpp` glob never compiles them into MonolithSource (a
// fixture TU in the module DLL is a full-unity symbol-collision candidate —
// issue #68 class). But the indexer DISCOVERS files by walking ProjectPath via
// FindFilesRecursive("*.cpp"), so a `.cpp.fixture` would be skipped and its
// source lines never reach source_fts (breaking FindExampleUsagePagination,
// which needs the .cpp's call-site lines).
//
// Bridge: STAGE a transient copy of the corpus, renaming every `*.cpp.fixture`
// back to `*.cpp` (bytes copied verbatim, so indexed CONTENT is byte-identical),
// preserving the Source/<Module>/ tree so DiscoverProjectModules finds it. Index
// the staged copy, then delete it.
// ---------------------------------------------------------------------------
namespace MonolithCppErgoTestDetail
{
	/** Recursively copy SrcDir -> DstDir, renaming `*.cpp.fixture` to `*.cpp`.
	 *  Bytes are copied verbatim. Returns false on any IO failure. */
	static bool StageCorpus(const FString& SrcDir, const FString& DstDir)
	{
		IFileManager& FM = IFileManager::Get();
		FM.MakeDirectory(*DstDir, /*Tree=*/true);

		TArray<FString> Files;
		FM.FindFilesRecursive(Files, *SrcDir, TEXT("*"), /*Files=*/true, /*Dirs=*/false, /*bClearFileNames=*/true);

		// Normalised corpus root for prefix stripping. FindFilesRecursive returns
		// FULL (absolute) paths (FileManagerGeneric bStoreFullPath=true), so the
		// relative path is the file path with the SrcDir prefix removed.
		//
		// NOTE: do NOT use FPaths::MakePathRelativeTo here — it internally applies
		// FPaths::GetPath() to the base argument (Paths.cpp:1525), stripping the
		// LAST component, so it computes paths relative to the corpus's PARENT, not
		// the corpus dir itself. That injected an extra "CppErgoCorpus/" segment,
		// landing staged files at StagedDir/CppErgoCorpus/Source/... while the
		// indexer walks StagedDir/Source/* — zero modules discovered, zero rows.
		FString NormSrcRoot = SrcDir;
		FPaths::NormalizeDirectoryName(NormSrcRoot);   // strips trailing slash, backslashes -> '/'

		for (const FString& SrcFile : Files)
		{
			FString NormSrc = SrcFile;
			FPaths::NormalizeFilename(NormSrc);        // backslashes -> '/'

			// Relative path = file path minus the "<corpus>/" prefix (case-insensitive
			// on Windows). If the prefix is absent (shouldn't happen) fall back to the
			// clean filename so the file still lands somewhere indexable.
			FString Rel;
			const FString Prefix = NormSrcRoot + TEXT("/");
			if (NormSrc.StartsWith(Prefix, ESearchCase::IgnoreCase))
			{
				Rel = NormSrc.RightChop(Prefix.Len());
			}
			else
			{
				Rel = FPaths::GetCleanFilename(NormSrc);
			}

			// Rename a trailing `.cpp.fixture` -> `.cpp` so the walker discovers it
			// and IndexCppFile classifies it as a "source" TU.
			if (Rel.EndsWith(TEXT(".cpp.fixture"), ESearchCase::IgnoreCase))
			{
				Rel.LeftChopInline(FCString::Strlen(TEXT(".fixture")));
			}

			const FString DstFile = DstDir / Rel;
			FM.MakeDirectory(*FPaths::GetPath(DstFile), /*Tree=*/true);

			// Verbatim byte copy (keeps indexed content byte-identical to the source).
			TArray<uint8> Bytes;
			if (!FFileHelper::LoadFileToArray(Bytes, *SrcFile)) { return false; }
			if (!FFileHelper::SaveArrayToFile(Bytes, *DstFile)) { return false; }
		}
		return true;
	}

	// Indexes the staged corpus into DbPath. On success, OutStagedDir receives the
	// transient staged-corpus path. The caller MUST delete OutStagedDir in teardown
	// AFTER closing the DB — the FTS-backed reads under test (find_example_usage
	// context read, SymbolExists FTS fallback, ResolveFirstSignature declaration-read)
	// RE-OPEN the staged .cpp/.h files on disk during assertions, so the staged dir
	// must outlive the DB. (Deleting it here, before assertions, was the zero-data bug.)
	static bool IndexFixtureCorpus(const FString& DbPath, FString& OutStagedDir, FString& OutError)
	{
		OutStagedDir.Empty();

		const FString CorpusDir = GetFixtureCorpusDir();
		if (!IFileManager::Get().DirectoryExists(*CorpusDir))
		{
			OutError = FString::Printf(TEXT("Fixture corpus not found: %s"), *CorpusDir);
			return false;
		}

		// Stage a transient copy with `.cpp.fixture` -> `.cpp` so the indexer's
		// FindFilesRecursive("*.cpp") walker picks up the fixture sources.
		const FString StagedDir = FPaths::AutomationTransientDir()
			/ FString::Printf(TEXT("cppergo-corpus-%s"), *FGuid::NewGuid().ToString());
		if (!StageCorpus(CorpusDir, StagedDir))
		{
			// Staging failed mid-way — best-effort clean up the partial dir.
			IFileManager::Get().DeleteDirectory(*StagedDir, /*bRequireExists=*/false, /*bTree=*/true);
			OutError = FString::Printf(TEXT("Failed to stage corpus into %s"), *StagedDir);
			return false;
		}

		FMonolithSourceIndexer Indexer;
		Indexer.SetSourcePath(TEXT(""));            // skip engine phase
		Indexer.SetShaderPath(TEXT(""));
		Indexer.SetProjectPath(StagedDir);          // StagedDir/Source/* discovered
		Indexer.SetDatabasePath(DbPath);
		Indexer.SetCleanBuild(true);
		Indexer.SetIndexProjectSource(true);
		const bool bRan = Indexer.RunSynchronous();

		if (!bRan)
		{
			// Index failed — staged files are no longer needed; clean up now.
			IFileManager::Get().DeleteDirectory(*StagedDir, /*bRequireExists=*/false, /*bTree=*/true);
			OutError = TEXT("Indexer.RunSynchronous() failed");
			return false;
		}

		// Success: hand the staged dir back so the caller can keep it alive through
		// the FTS-backed assertions and delete it in teardown after the DB close.
		OutStagedDir = StagedDir;
		return true;
	}
}

// ---------------------------------------------------------------------------
// Phase 2 Test: VerifySymbolsComposition — the class-body method MUST report
// exists:true (resolved via class row + source_fts declaration hit, NOT
// symbols-table presence); a missing symbol reports exists:false; a deprecated
// symbol resolves its row. Exercises the shared composition helpers directly.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCppErgoVerifySymbolsCompositionTest,
	"Monolith.Source.CppErgonomics.VerifySymbolsComposition",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCppErgoVerifySymbolsCompositionTest::RunTest(const FString& /*Parameters*/)
{
	using namespace MonolithCppErgoTestDetail;

	const FString DbPath = MakeTempDbPath();
	FString StagedDir;
	FString Err;
	if (!IndexFixtureCorpus(DbPath, StagedDir, Err))
	{
		AddError(Err);
		IFileManager::Get().Delete(*DbPath, false, true);
		return false;
	}

	FMonolithSourceDatabase DB;
	if (!DB.Open(DbPath))
	{
		AddError(TEXT("Failed to reopen indexed DB"));
		IFileManager::Get().Delete(*DbPath, false, true);
		IFileManager::Get().DeleteDirectory(*StagedDir, /*bRequireExists=*/false, /*bTree=*/true);
		return false;
	}

	// UCppErgoUsage::CallMe is a class-body method (NO symbols row). Existence must
	// resolve via the owning class row + source_fts declaration hit.
	TestTrue(TEXT("class-body method exists:true"),
		FMonolithSourceActions::SymbolExists(&DB, TEXT("UCppErgoUsage::CallMe")));

	// Owning class itself exists.
	TestTrue(TEXT("class exists:true"),
		FMonolithSourceActions::SymbolExists(&DB, TEXT("UCppErgoUsage")));

	// A symbol absent from the corpus reports exists:false (no error).
	TestFalse(TEXT("missing symbol exists:false"),
		FMonolithSourceActions::SymbolExists(&DB, TEXT("UThisDoesNotExistAnywhere::Nope")));

	// Signature resolution for the class-body method comes from declaration_read.
	{
		FString Sig, Source;
		const bool bOk = FMonolithSourceActions::ResolveFirstSignature(&DB, TEXT("UCppErgoUsage::CallMe"), Sig, Source);
		TestTrue(TEXT("CallMe signature resolved"), bOk);
		if (bOk)
		{
			TestTrue(TEXT("CallMe signature contains CallMe("), Sig.Contains(TEXT("CallMe(")));
			TestFalse(TEXT("CallMe signature has no body"), Sig.Contains(TEXT("{")));
		}
	}

	// Deprecation composition: UDeprecatedThings::Foo is UE_DEPRECATED(5.4, ...).
	{
		TOptional<FMonolithDeprecationRow> Dep = DB.GetDeprecation(TEXT("Foo"));
		TestTrue(TEXT("Foo deprecation row present"), Dep.IsSet());
	}

	// Teardown: close the DB FIRST, then delete the staged corpus. The assertions
	// above re-open the staged .cpp/.h files (FTS-backed reads), so the staged dir
	// must outlive every assertion.
	DB.Close();
	IFileManager::Get().Delete(*DbPath, false, true);
	IFileManager::Get().DeleteDirectory(*StagedDir, /*bRequireExists=*/false, /*bTree=*/true);
	return true;
}

// ---------------------------------------------------------------------------
// Phase 2 Test: FindExampleUsagePagination — the FTS substrate yields >limit
// distinct call-site lines for `CallMe(`; the rerun-slice + MonolithCursorCodec
// cursor round-trips (page 0 emits next_cursor, decode threads page 1, page 1
// returns the remaining rows and no further cursor). Reproduces the handler's
// FTS + slice + cursor logic against the disposable DB (the JSON handler routes
// through GetDB()/GEditor, which the test cannot supply).
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCppErgoFindExampleUsagePaginationTest,
	"Monolith.Source.CppErgonomics.FindExampleUsagePagination",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCppErgoFindExampleUsagePaginationTest::RunTest(const FString& /*Parameters*/)
{
	using namespace MonolithCppErgoTestDetail;

	const FString DbPath = MakeTempDbPath();
	FString StagedDir;
	FString Err;
	if (!IndexFixtureCorpus(DbPath, StagedDir, Err))
	{
		AddError(Err);
		IFileManager::Get().Delete(*DbPath, false, true);
		return false;
	}

	FMonolithSourceDatabase DB;
	if (!DB.Open(DbPath))
	{
		AddError(TEXT("Failed to reopen indexed DB"));
		IFileManager::Get().Delete(*DbPath, false, true);
		IFileManager::Get().DeleteDirectory(*StagedDir, /*bRequireExists=*/false, /*bTree=*/true);
		return false;
	}

	// Gather all distinct call-site lines matching `CallMe(` via source_fts —
	// the same substrate the handler uses.
	const FString Symbol = TEXT("CallMe");
	const FString Needle = Symbol + TEXT("(");
	TArray<TPair<FString, int32>> Hits; // (file, 1-based line)
	TSet<FString> Seen;

	TArray<FMonolithSourceChunk> Chunks = DB.SearchSourceFTS(Symbol, TEXT("all"), 400);
	for (const FMonolithSourceChunk& Chunk : Chunks)
	{
		const FString FilePath = DB.GetFilePath(Chunk.FileId);
		TArray<FString> FileLines;
		if (!FFileHelper::LoadFileToStringArray(FileLines, *FilePath)) continue;
		const int32 WinStart = FMath::Max(0, Chunk.LineNumber - 1);
		const int32 WinEnd = FMath::Min(FileLines.Num(), WinStart + 10);
		for (int32 i = WinStart; i < WinEnd; ++i)
		{
			const FString& L = FileLines[i];
			const int32 Idx = L.Find(Needle, ESearchCase::CaseSensitive);
			if (Idx == INDEX_NONE) continue;
			if (Idx > 0)
			{
				const TCHAR Prev = L[Idx - 1];
				if (FChar::IsAlnum(Prev) || Prev == TEXT('_')) continue;
			}
			const FString Key = FString::Printf(TEXT("%lld_%d"), Chunk.FileId, i + 1);
			if (Seen.Contains(Key)) continue;
			Seen.Add(Key);
			Hits.Add(TPair<FString, int32>(FilePath, i + 1));
		}
	}
	// Deterministic order (handler ranks then tie-breaks by path+line).
	Hits.Sort([](const TPair<FString, int32>& A, const TPair<FString, int32>& B)
	{
		if (A.Key != B.Key) return A.Key < B.Key;
		return A.Value < B.Value;
	});

	const int32 Total = Hits.Num();
	// The fixture .cpp/.h together carry >10 `CallMe(` declaration+call lines.
	TestTrue(TEXT("more than one page of hits"), Total > 5);

	const int32 Limit = 5;
	const uint32 QHash = MonolithCursorCodec::ComputeQueryHash(
		Symbol, TEXT("engine"), TEXT("find_example_usage"), TEXT(""), TEXT(""), TEXT(""));

	// Page 0.
	const int32 P0Start = 0;
	const int32 P0End = FMath::Min(P0Start + Limit, Total);
	const int32 P0Rows = P0End - P0Start;
	TestEqual(TEXT("page 0 full"), P0Rows, Limit);

	FString NextCursor;
	if (P0Rows >= Limit && (P0Start + Limit) < Total)
	{
		MonolithCursorCodec::FCursorState OutState;
		OutState.QueryHash = QHash;
		OutState.SymbolPage = 1;
		OutState.SourcePage = 1;
		OutState.CachedTotalEstimate = Total;
		NextCursor = MonolithCursorCodec::Encode(OutState);
	}
	TestFalse(TEXT("page 0 emits next_cursor"), NextCursor.IsEmpty());

	// Decode the cursor and fetch page 1.
	MonolithCursorCodec::FCursorState Decoded;
	const bool bDecoded = MonolithCursorCodec::Decode(NextCursor, Decoded);
	TestTrue(TEXT("cursor decodes"), bDecoded);
	TestEqual(TEXT("cursor query hash matches"), Decoded.QueryHash, QHash);
	TestEqual(TEXT("cursor page index 1"), Decoded.SourcePage, 1);

	const int32 P1Start = Decoded.SourcePage * Limit;
	const int32 P1End = FMath::Min(P1Start + Limit, Total);
	const int32 P1Rows = FMath::Max(0, P1End - P1Start);
	TestTrue(TEXT("page 1 returns rows"), P1Rows > 0);

	// Page 0 and page 1 must be disjoint (no overlap in the sliced indices).
	TestTrue(TEXT("page 1 starts after page 0"), P1Start >= P0End);

	// Teardown: close the DB FIRST, then delete the staged corpus. The hit-gathering
	// loop above re-opens the staged .cpp/.h files (LoadFileToStringArray), so the
	// staged dir must outlive every read.
	DB.Close();
	IFileManager::Get().Delete(*DbPath, false, true);
	IFileManager::Get().DeleteDirectory(*StagedDir, /*bRequireExists=*/false, /*bTree=*/true);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
