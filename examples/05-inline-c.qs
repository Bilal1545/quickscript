let n = 1000000

let jsSum = 0
for (let i = 1; i <= n; i++) {
    jsSum += i
}
print("QS-side sum 1..", n, "=", jsSum)

__c {{{
    long long sum = 0;
    long long N = (long long)n->number;
    for (long long i = 1; i <= N; i++) {
        sum += i;
    }
    printf("C-side  sum 1..%lld = %lld\n", N, sum);
}}}

print("done.")
