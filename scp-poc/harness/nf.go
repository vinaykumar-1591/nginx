// Mock 5G Network Function (NF) producer for the nginx-scp demo.
//
// It serves cleartext HTTP/2 (h2c) so the SCP can reach it via
// `proxy_http_version 2`. Each instance reports its own id so load balancing
// and reselection across an NF set are visible in responses and logs.
//
//	go run nf.go -addr 127.0.0.1:9101 -id udm-1 -nf UDM
package main

import (
	"flag"
	"fmt"
	"log"
	"net/http"
	"os"

	"golang.org/x/net/http2"
	"golang.org/x/net/http2/h2c"
)

func main() {
	addr := flag.String("addr", "127.0.0.1:9101", "listen address")
	id := flag.String("id", "nf-1", "instance id")
	nf := flag.String("nf", "UDM", "NF type")
	flag.Parse()

	h := func(w http.ResponseWriter, r *http.Request) {
		log.Printf("[%s/%s] %s %s proto=%s target=%q binding=%q",
			*nf, *id, r.Method, r.URL.Path, r.Proto,
			r.Header.Get("3gpp-Sbi-Target-apiRoot"),
			r.Header.Get("3gpp-Sbi-Routing-Binding"))

		w.Header().Set("Content-Type", "application/json")
		w.Header().Set("X-Nf-Instance", *id)
		w.Header().Set("3gpp-Sbi-Producer-Id", *id)
		w.WriteHeader(http.StatusOK)
		fmt.Fprintf(w, `{"nf":"%s","instance":"%s","proto":"%s","path":"%s"}`+"\n",
			*nf, *id, r.Proto, r.URL.Path)
	}

	// Wrap the handler so it answers h2c (cleartext HTTP/2).
	srv := &http.Server{
		Addr:    *addr,
		Handler: h2c.NewHandler(http.HandlerFunc(h), &http2.Server{}),
	}

	log.Printf("mock NF %s/%s listening (h2c) on %s", *nf, *id, *addr)
	if err := srv.ListenAndServe(); err != nil {
		log.Printf("server error: %v", err)
		os.Exit(1)
	}
}
