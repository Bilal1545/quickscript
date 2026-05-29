function label(i) {
    if (i % 15 === 0) { return "FizzBuzz" }
    if (i % 3 === 0)  { return "Fizz" }
    if (i % 5 === 0)  { return "Buzz" }
    return i
}

let limit = 20
if (process.argv.length > 1) {
    limit = Number(process.argv[1])
}

for (let i = 1; i <= limit; i++) {
    print(label(i))
}
