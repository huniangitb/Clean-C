module.exports = {
	     plugins: {
		            '@fullhuman/postcss-purgecss': {
				             content: ['./src/**/*.html', './src/**/*.js'],
				             css: ['./src/**/*.css'],
				             safelist: {
						                standard: [/active/, /show/, /fade/],
						                deep: [/modal/, /tooltip/, /popover/]
						              }
				           }
		          }
	   };
