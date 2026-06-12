import qualified Data.Sequence as Seq
import Data.Sequence (Seq)
import Data.Char (ord)
import qualified Data.Foldable as Foldable

-- Your function
filterAscii :: Seq Char -> Seq Char
filterAscii s = go 0 s
  where
    go i s
      | i >= Seq.length s = s
      | otherwise =
          let c = Seq.index s i
          in if ord c >= 0x7F
             then go i (Seq.deleteAt i s)
             else go (i + 1) s

-- Reference implementation (for testing)
filterAsciiRef :: String -> String
filterAsciiRef = filter (\c -> ord c < 0x7F)

-- Convert Seq to String
toString :: Seq Char -> String
toString = Foldable.toList

-- Test runner
runTest :: String -> IO ()
runTest input = do
    let seqInput = Seq.fromList input
        result   = toString (filterAscii seqInput)
        expected = filterAsciiRef input

    putStrLn $ "Input:    " ++ show input
    putStrLn $ "Expected: " ++ show expected
    putStrLn $ "Result:   " ++ show result

    if result == expected
       then putStrLn "✅ PASS\n"
       else putStrLn "❌ FAIL\n"

main :: IO ()
main = do
    putStrLn "Running tests...\n"

    -- Basic cases
    runTest ""
    runTest "hello"
    runTest "abc\x7Fdef"         -- DEL char
    runTest "abc\x80def"         -- non-ASCII
    runTest "héllo"              -- accented
    runTest "こんにちは"         -- Japanese
    runTest "ASCII only 123!"

    -- Mixed cases
    runTest "a\x7Fb\x80c\x90d"
