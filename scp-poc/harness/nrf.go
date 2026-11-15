// Mock 5G NRF (Network Repository Function) for the nginx-scp delegated
// discovery demo. It implements a tiny slice of Nnrf_NFDiscovery
// (TS 29.510): given a target NF type, it returns the NF-set to route to.
//
// The SCP calls this via an auth_request subrequest; the chosen NF-set host is
// returned in the X-Nrf-Target-Host response header, which the SCP uses as the
// upstream for the actual (HTTP/2) request.
//
//	go run nrf.go -addr 127.0.0.1:9301
package main

import (
	"flag"
	"fmt"
	"log"
	"net/http"
	"strings"
)

// NF type -> (NF-set upstream host, representative apiRoot).
var registry = map[string]string{
	"UDM": "udm-set",
	"AMF": "amf-set",
}

func main() {
	addr := flag.String("addr", "127.0.0.1:9301", "listen address")
	flag.Parse()

	http.HandleFunc("/nnrf-disc/v1/nf-instances", func(w http.ResponseWriter, r *http.Request) {
		nfType := r.Header.Get("3gpp-Sbi-Discovery-target-nf-type")
		if nfType == "" {
			nfType = r.URL.Query().Get("target-nf-type")
		}
		nfType = strings.ToUpper(strings.TrimSpace(nfType))

		host, ok := registry[nfType]
		if !ok {
			log.Printf("NRF discovery: target-nf-type=%q -> NOT FOUND", nfType)
			w.Header().Set("Content-Type", "application/problem+json")
			// 403 so the SCP's auth_request treats it as a clean deny.
			w.WriteHeader(http.StatusForbidden)
			fmt.Fprintf(w, `{"title":"Forbidden","status":403,`+
				`"cause":"NF_DISCOVERY_FAILURE",`+
				`"detail":"no NF instance for target-nf-type %q"}`+"\n", nfType)
			return
		}

		log.Printf("NRF discovery: target-nf-type=%q -> nf-set=%q", nfType, host)

		apiRoot := "http://" + host
		w.Header().Set("Content-Type", "application/json")
		w.Header().Set("X-Nrf-Target-Host", host)       // consumed by the SCP
		w.Header().Set("3gpp-Sbi-Target-apiRoot", apiRoot)
		w.WriteHeader(http.StatusOK)
		// A minimal Nnrf_NFDiscovery SearchResult (TS 29.510).
		fmt.Fprintf(w, `{"validityPeriod":3600,"nfInstances":[`+
			`{"nfInstanceId":"%s-01","nfType":"%s","nfStatus":"REGISTERED",`+
			`"nfServices":[{"serviceInstanceId":"svc-1","scheme":"http",`+
			`"apiPrefix":"%s"}]}]}`+"\n", nfType, nfType, apiRoot)
	})

	log.Printf("mock NRF listening on %s", *addr)
	if err := http.ListenAndServe(*addr, nil); err != nil {
		log.Fatalf("server error: %v", err)
	}
}
