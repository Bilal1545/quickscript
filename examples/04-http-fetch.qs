let url = "http://example.com/"
if (process.argv.length > 1) {
    url = process.argv[1]
}

print("GET", url)
let r = http.get(url)

print("status     :", r.status, r.statusText)
print("content-type:", r.headers["content-type"])
print("length     :", r.body.length, "bytes")
print("---")
print(r.body.slice(0, 200))
