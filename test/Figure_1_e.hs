import Data.Char (ord)

-- Implementation under test
filterAscii :: String -> String
filterAscii [] = []
filterAscii (c:cs)
  | ord c < 0x7F = c : filterAscii cs
  | otherwise    = filterAscii cs

-- Reference implementation
filterAsciiRef :: String -> String
filterAsciiRef = filter (\c -> ord c < 0x7F)

-- Test runner
runTest :: String -> IO ()
runTest input = do
    let result   = filterAscii input
        expected = filterAsciiRef input

    putStrLn $ "Input:    " ++ show input
    putStrLn $ "Expected: " ++ show expected
    putStrLn $ "Result:   " ++ show result

    if result == expected
       then putStrLn "✅ PASS\n"
       else putStrLn "❌ FAIL\n"

main :: IO ()
main = do
    putStrLn "Running tests (recursive version)...\n"

    runTest ""
    runTest "hello"
    runTest "abc\x7Fdef"
    runTest "abc\x80def"
    runTest "héllo"
    runTest "こんにちは"
    runTest "ASCII only 123!"
    runTest "a\x7Fb\x80c\x90d"
