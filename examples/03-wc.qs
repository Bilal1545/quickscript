if (process.argv.length < 2) {
    print("usage: wc <file>")
    process.exit(1)
}

let file = process.argv[1]
if (!fs.exists(file)) {
    print("no such file:", file)
    process.exit(1)
}

let body = fs.readFile(file)

let lines = 0
let words = 0
let chars = body.length
let inWord = false

for (let i = 0; i < body.length; i++) {
    let ch = body[i]
    if (ch === "\n") {
        lines++
    }
    let isSpace = (ch === " " || ch === "\t" || ch === "\n" || ch === "\r")
    if (isSpace) {
        inWord = false
    } else {
        if (!inWord) {
            words++
            inWord = true
        }
    }
}

print(lines, words, chars, file)
